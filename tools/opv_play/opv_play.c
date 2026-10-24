/*
 * opv_play.c -- VCK190 memory loopback: play a stimulus through the Haifuraiya
 * receiver in the fabric and capture its soft bits.
 *
 *   DDR --axi_dma MM2S--> axis_sample_pacer (20 MSps) --> haifuraiya_rx_axi
 *                         --> axis_softbit_widen --axi_dma S2MM--> DDR
 *
 * Registers: /dev/uio0 (generic-uio), map0 = channelizer control block
 * (axi_lite_regs, 0xA4010000), map1 = demod control block
 * (haifuraiya_demod_regs, 0xA4020000).  The bring-up sequence below is the
 * simulation bench's (tb_haifuraiya_channelizer_axi.vhd, Phase 2), register
 * for register, so the board gets the configuration the acceptance run gets.
 *
 * DMA: Xilinx dma-proxy (/dev/dma_proxy_tx, /dev/dma_proxy_rx; dma-proxy.h
 * from the PetaLinux recipe, BUFFER_SIZE 4 MB x 32 per channel).  The whole
 * stimulus is queued on the transmit ring up front; the receive ring takes
 * one frame per buffer (FRAME_SOFTS bytes, terminated by the receiver's
 * TLAST), so no refill thread is needed.  The receiver is sample-driven:
 * pauses in delivery are invisible to it, only order and completeness
 * matter, and a scatter-gather DMA guarantees both.
 *
 * Input : packed little-endian int16 I, int16 Q per sample (one 32-bit beat,
 *         I in the low half -- the AXIS convention the channelizer uses),
 *         made on the host from opv_stim's text by opv_stim_txt2bin.py.
 * Output: one byte per soft bit (0..7), frames back to back, converted on
 *         the host to the grader's one-value-per-line text by
 *         opv_soft_bin2txt.py.
 *
 * Usage: opv_play -i stim.bin -o soft.bin [-f max_frames] [-q] [-v]
 *   -f  stop after this many frames (default: until 3 s with no frame)
 *   -q  print the readbacks (CFO estimate, symbol clock offset, frames) at
 *       the end
 *   -v  trace each DMA step
 *
 * Exit status 0 on a clean run, 1 on a usage/IO error, 2 on a DMA error.
 *
 * Build: cross for aarch64 (see Makefile); runs as root on the board.
 * License: CERN-OHL-S-2.0 (Open Research Institute), dma-proxy.h Apache-2.0.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "dma-proxy.h"

/* ---- register maps (offsets in bytes) -------------------------------- */
/* channelizer (axi_lite_regs.vhd) */
#define CH_VERSION       0x000
#define CH_CONTROL       0x004   /* bit0 soft reset, bit1 enable */
#define CH_OUTPUT_SHIFT  0x014
#define CH_ALPHA1        0x018
#define CH_ALPHA2        0x01C
#define CH_NORM_SQUELCH  0x020
/* demod (haifuraiya_demod_regs.vhd, map v6) */
#define DM_VERSION       0x000
#define DM_CONTROL       0x004
#define DM_FREQ_F1       0x008
#define DM_FREQ_F2       0x00C
#define DM_LPF_P_GAIN    0x010
#define DM_LPF_I_GAIN    0x014
#define DM_LPF_ALPHA     0x018
#define DM_LPF_P_SHIFT   0x01C
#define DM_LPF_I_SHIFT   0x020
#define DM_SYM_LOCK_CNT  0x024
#define DM_SYM_LOCK_THR  0x028
#define DM_GAIN_MANUAL   0x030
#define DM_STATUS        0x040   /* bit0 frame_sync_locked, bit1 sym lock */
#define DM_FRAMES_RX     0x044
#define DM_FS_HUNT_PCT   0x048
#define DM_FS_VERIFY_PCT 0x04C
#define DM_QUANT_THR_1   0x050
#define DM_QUANT_THR_2   0x054
#define DM_QUANT_THR_3   0x058
#define DM_INIT          0x05C
#define DM_LOOP_CTRL     0x060
#define DM_RX_DISCARD    0x064
#define DM_SYM_LOCK_PCT  0x0A4
#define DM_SYM_UNLOCK_PCT 0x0A8
#define DM_SYM_LOCK_WIN  0x0AC
#define DM_CFO_STATE     0x0B0
#define DM_CFO_ESTIMATE  0x0B4
#define DM_CFO_CTRL      0x0B8
#define DM_CFO_MANUAL    0x0BC
#define DM_CFO_QUALITY   0x0C0
#define DM_TIM_ALPHA     0x0C4
#define DM_TIM_BETA      0x0C8
#define DM_SYM_CLK_OFF   0x0CC
#define DM_SOFT_DROPPED  0x0D0

#define FRAME_SOFTS      2144    /* soft bits per frame (sync + payload) = bytes per RX transfer */
#define RX_IDLE_MS       3000    /* dma-proxy's own wait timeout; one timed-out RX = end of run */

static int verbose = 0;

static volatile uint32_t *map_uio(int fd, int map, size_t len)
{
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                   (off_t)map * (off_t)sysconf(_SC_PAGESIZE));
    if (p == MAP_FAILED) { perror("mmap uio"); return NULL; }
    return (volatile uint32_t *)p;
}
static inline void wr(volatile uint32_t *base, uint32_t off, uint32_t v)
{
    base[off / 4] = v;
    if (verbose) fprintf(stderr, "  wr 0x%03X <= 0x%08X\n", off, v);
}
static inline uint32_t rd(volatile uint32_t *base, uint32_t off) { return base[off / 4]; }

/* the bench's Phase 2 bring-up, verbatim (tb_haifuraiya_channelizer_axi.vhd) */
static void configure_receiver(volatile uint32_t *ch, volatile uint32_t *dm)
{
    /* channelizer */
    wr(ch, CH_OUTPUT_SHIFT, 14);        /* match the board: 4x more channel amplitude than default 16 */
    wr(ch, CH_ALPHA1, 4096);            /* tau1 = 64 samples = 0.10 ms */
    wr(ch, CH_ALPHA2, 256);             /* tau2 = 1024 samples = 1.64 ms */
    wr(ch, CH_NORM_SQUELCH, 655360);    /* ~2x the noise floor at OUTPUT_SHIFT 14 (register default) */
    wr(ch, CH_CONTROL, 1);              /* soft reset: clears the EMA cascade */
    wr(ch, CH_CONTROL, 2);              /* release + enable */
    usleep(10);
    /* demod */
    wr(dm, DM_INIT, 0x00000001);        /* DEMOD_INIT = 1: hold the loops in reset */
    wr(dm, DM_CONTROL, 0x00000000);     /* rx_invert = 0 */
    wr(dm, DM_FREQ_F1, 0xFA732DF5);     /* -13550 Hz @ 625 ksps */
    wr(dm, DM_FREQ_F2, 0x058CD20B);     /* +13550 Hz */
    wr(dm, DM_LPF_P_GAIN, 0x33);
    wr(dm, DM_LPF_I_GAIN, 0x07);
    wr(dm, DM_LPF_ALPHA, 0x00);
    wr(dm, DM_LPF_P_SHIFT, 0x02);
    wr(dm, DM_LPF_I_SHIFT, 0x0C);
    wr(dm, DM_SYM_LOCK_CNT, 0x80);      /* 128 */
    wr(dm, DM_SYM_LOCK_THR, 0x08);
    wr(dm, DM_GAIN_MANUAL, 0x400);      /* 1.000 (Q6.10) */
    wr(dm, DM_RX_DISCARD, 0x00);
    wr(dm, DM_SYM_LOCK_PCT, 25);        /* C++ LOCK_THRESH 0.25 */
    wr(dm, DM_SYM_UNLOCK_PCT, 50);      /* C++ UNLOCK_THRESH 0.50 */
    wr(dm, DM_SYM_LOCK_WIN, 6);         /* log2 -> 64 symbols */
    wr(dm, DM_CFO_CTRL, 0x00060A01);    /* auto */
    wr(dm, DM_CFO_MANUAL, 0);
    wr(dm, DM_TIM_ALPHA, 0x0500);
    wr(dm, DM_TIM_BETA, 0x0022);
    wr(dm, DM_QUANT_THR_1, 4942);
    wr(dm, DM_QUANT_THR_2, 9884);
    wr(dm, DM_QUANT_THR_3, 14826);
    wr(dm, DM_FS_HUNT_PCT, 85);
    wr(dm, DM_FS_VERIFY_PCT, 70);
    wr(dm, DM_LOOP_CTRL, 0x00000004);   /* rx_enable = 1, not frozen/zeroed */
    wr(dm, DM_INIT, 0x00000000);        /* release onto the live channel output */
}

static void readbacks(volatile uint32_t *ch, volatile uint32_t *dm)
{
    uint32_t st = rd(dm, DM_STATUS);
    int32_t  cfo = (int16_t)(rd(dm, DM_CFO_ESTIMATE) & 0xFFFF);
    int32_t  clk = (int32_t)rd(dm, DM_SYM_CLK_OFF);
    printf("channelizer VERSION 0x%08X   demod VERSION 0x%08X\n", rd(ch, CH_VERSION), rd(dm, DM_VERSION));
    printf("DEMOD_STATUS 0x%08X  (frame_sync_locked=%u sym_locked=%u)\n", st, st & 1, (st >> 1) & 1);
    printf("FRAMES_RECEIVED %u   SOFT_DROPPED %u\n", rd(dm, DM_FRAMES_RX), rd(dm, DM_SOFT_DROPPED));
    printf("CFO_STATE %u  CFO_ESTIMATE %d Hz  CFO_QUALITY %u\n",
           rd(dm, DM_CFO_STATE) & 7, cfo, rd(dm, DM_CFO_QUALITY) & 0xFFFF);
    printf("SYM_CLK_OFFSET %d (Q24 samples/symbol = %.2f ppm)\n", clk,
           (double)clk / 16777216.0 / 11.5314 * 1e6);
}

static int usage(const char *a)
{
    fprintf(stderr, "usage: %s -i stim.bin -o soft.bin [-f max_frames] [-q] [-v]\n", a);
    return 1;
}

int main(int argc, char **argv)
{
    const char *in_path = NULL, *out_path = NULL;
    int max_frames = 0, want_readbacks = 0, opt;
    while ((opt = getopt(argc, argv, "i:o:f:qv")) != -1) {
        switch (opt) {
        case 'i': in_path = optarg; break;
        case 'o': out_path = optarg; break;
        case 'f': max_frames = atoi(optarg); break;
        case 'q': want_readbacks = 1; break;
        case 'v': verbose = 1; break;
        default: return usage(argv[0]);
        }
    }
    if (!in_path || !out_path) return usage(argv[0]);

    /* ---- stimulus ---- */
    int in_fd = open(in_path, O_RDONLY);
    if (in_fd < 0) { perror(in_path); return 1; }
    struct stat sb;
    if (fstat(in_fd, &sb) < 0) { perror("fstat"); return 1; }
    size_t in_bytes = (size_t)sb.st_size & ~(size_t)3;     /* whole beats */
    size_t n_tx = (in_bytes + BUFFER_SIZE - 1) / BUFFER_SIZE;
    if (n_tx > BUFFER_COUNT) {
        fprintf(stderr, "stimulus %zu bytes needs %zu buffers; the transmit ring holds %d x %d bytes\n",
                in_bytes, n_tx, BUFFER_COUNT, BUFFER_SIZE);
        return 1;
    }
    fprintf(stderr, "stimulus: %zu bytes = %zu samples = %.3f s at 20 MSps, %zu DMA buffers\n",
            in_bytes, in_bytes / 4, (double)(in_bytes / 4) / 20e6, n_tx);

    /* ---- registers ---- */
    int uio = open("/dev/uio0", O_RDWR | O_SYNC);
    if (uio < 0) { perror("/dev/uio0"); return 1; }
    volatile uint32_t *ch = map_uio(uio, 0, 0x10000);
    volatile uint32_t *dm = map_uio(uio, 1, 0x10000);
    if (!ch || !dm) return 1;
    if (rd(ch, CH_VERSION) != 0x00010000)
        fprintf(stderr, "warning: channelizer VERSION 0x%08X (expected 0x00010000)\n", rd(ch, CH_VERSION));

    /* ---- DMA rings ---- */
    int tx_fd = open("/dev/dma_proxy_tx", O_RDWR);
    int rx_fd = open("/dev/dma_proxy_rx", O_RDWR);
    if (tx_fd < 0 || rx_fd < 0) { perror("/dev/dma_proxy_*"); return 1; }
    struct channel_buffer *tx = mmap(NULL, sizeof(struct channel_buffer) * BUFFER_COUNT,
                                     PROT_READ | PROT_WRITE, MAP_SHARED, tx_fd, 0);
    struct channel_buffer *rx = mmap(NULL, sizeof(struct channel_buffer) * BUFFER_COUNT,
                                     PROT_READ | PROT_WRITE, MAP_SHARED, rx_fd, 0);
    if (tx == MAP_FAILED || rx == MAP_FAILED) { perror("mmap dma_proxy"); return 1; }

    /* load the stimulus into the transmit ring */
    size_t left = in_bytes;
    for (size_t b = 0; b < n_tx; b++) {
        size_t chunk = left > BUFFER_SIZE ? BUFFER_SIZE : left;
        size_t got = 0;
        while (got < chunk) {
            ssize_t r = read(in_fd, (char *)tx[b].buffer + got, chunk - got);
            if (r <= 0) { perror("read stimulus"); return 1; }
            got += (size_t)r;
        }
        tx[b].length = (unsigned int)chunk;
        left -= chunk;
    }
    close(in_fd);

    /* ---- configure, arm receive, start transmit ---- */
    configure_receiver(ch, dm);

    FILE *out = fopen(out_path, "wb");
    if (!out) { perror(out_path); return 1; }

    for (int b = 0; b < BUFFER_COUNT; b++) {
        rx[b].length = FRAME_SOFTS;
        int id = b;
        if (ioctl(rx_fd, START_XFER, &id) < 0) { perror("START_XFER rx"); return 2; }
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t b = 0; b < n_tx; b++) {
        int id = (int)b;
        if (ioctl(tx_fd, START_XFER, &id) < 0) { perror("START_XFER tx"); return 2; }
        if (verbose) fprintf(stderr, "  tx buffer %zu started (%u bytes)\n", b, tx[b].length);
    }

    /* ---- collect frames as they land; re-arm each buffer behind us ---- */
    int frames = 0, rx_id = 0, done = 0;
    while (!done) {
        int id = rx_id;
        if (ioctl(rx_fd, FINISH_XFER, &id) < 0) { perror("FINISH_XFER rx"); return 2; }
        if (rx[rx_id].status == PROXY_NO_ERROR) {
            fwrite(rx[rx_id].buffer, 1, FRAME_SOFTS, out);
            frames++;
            if (verbose) fprintf(stderr, "  frame %d (rx buffer %d)\n", frames, rx_id);
            rx[rx_id].length = FRAME_SOFTS;
            if (ioctl(rx_fd, START_XFER, &id) < 0) { perror("START_XFER rx re-arm"); return 2; }
            rx_id = (rx_id + 1) % BUFFER_COUNT;
            if (max_frames && frames >= max_frames) done = 1;
        } else if (rx[rx_id].status == PROXY_TIMEOUT) {
            done = 1;                       /* RX_IDLE_MS with no frame: the run is over */
        } else {
            fprintf(stderr, "rx buffer %d: DMA error status %d\n", rx_id, rx[rx_id].status);
            fclose(out);
            return 2;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fclose(out);

    /* drain the transmit completions (all should be long done) */
    for (size_t b = 0; b < n_tx; b++) {
        int id = (int)b;
        ioctl(tx_fd, FINISH_XFER, &id);
        if (tx[b].status != PROXY_NO_ERROR)
            fprintf(stderr, "warning: tx buffer %zu status %d\n", b, tx[b].status);
    }
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    fprintf(stderr, "%d frames (%d soft bits) in %.2f s (includes the %d ms end-of-run timeout)\n",
            frames, frames * FRAME_SOFTS, secs, RX_IDLE_MS);
    if (want_readbacks) readbacks(ch, dm);
    return 0;
}
