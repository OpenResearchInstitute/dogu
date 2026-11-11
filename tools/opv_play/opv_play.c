/*
 * opv_play.c -- VCK190 memory loopback: play a stimulus through the Haifuraiya
 * receiver in the fabric and capture its soft bits.
 *
 *   DDR --axi_dma MM2S--> axis_sample_pacer (20 MSps) --> haifuraiya_rx_axi
 *                         --> axis_softbit_widen --axi_dma S2MM--> DDR
 *
 * Registers: the receiver's UIO device (generic-uio, found by name
 * "haifuraiya_rx_axi" under /sys/class/uio -- never by number: the kernel
 * numbers UIO devices in device-tree order, and the tier-2 image's link
 * block sorts before the receiver, 2026-11-10), map0 = channelizer control
 * block (axi_lite_regs, 0xA4010000), map1 = demod control block
 * (haifuraiya_demod_regs, 0xA4020000).  The bring-up sequence below is the
 * simulation bench's (tb_haifuraiya_channelizer_axi.vhd, Phase 2), register
 * for register, so the board gets the configuration the acceptance run gets.
 *
 * Tier 2 (digital loopback): the ESIstream link block (esi_link_hw,
 * 0xA4030000) sits between the pacer and the receiver and is the UIO device
 * named "esi_link_hw", map0.  When it is present, opv_play refuses to play until the link
 * reports ready and prints the link's own scoreboard with the readbacks:
 * samples sent and received through the link and the three error counters.
 * Pass for tier 2 = the tier-1 grade AND sent = received = the stimulus
 * length AND zero errors.  -l reads the link and exits, without playing.
 * Register map: haifuraiya/platforms/vck190/rtl/esi/esi_link_hw.vhd.
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
 * Usage: opv_play -i stim.bin -o soft.bin [-f max_frames] [-L rx_bytes] [-q] [-v]
 *        opv_play -l
 *   -L  bytes per receive transfer (default FRAME_SOFTS = one frame).  Debug
 *       knob, 2026-10-25: the S2MM channel wedged after a frame that filled
 *       its buffer exactly (2144 bytes + TLAST on the same beat).  4194304
 *       makes the DMA end on TLAST only; 1072 makes it end on count only.
 *   -f  stop after this many frames (default: until 3 s with no frame)
 *   -q  print the readbacks (CFO estimate, symbol clock offset, frames) at
 *       the end -- and the ESIstream link's scoreboard when the link is there
 *   -l  tier 2: print the ESIstream link status and counters, then exit
 *   -r  tier 2: pulse the transceiver reset first, wait for it, then as -l.
 *       Needed once per boot when the GT reference clock is programmed by
 *       Linux (the SI570 node in the device tree) after the fabric's own
 *       power-up reset ran against the oscillator's 100 MHz default.
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

/* ESIstream link block (esi_link_hw.vhd), tier 2 only, UIO "esi_link_hw" map0 */
#define ESI_ID           0x000   /* 0x45534931 "ESI1" */
#define ESI_CONTROL      0x004   /* bit0 link reset, bit1 sync req, bit2 stats clear, bit3 GT reset,
                                    bits 6:4 loopback mode, bit7 snapshot (pulse bits read 0) */
#define ESI_STATUS       0x008   /* see esi_status_bits[] */
#define ESI_SAMPLES_SENT 0x00C   /* snapshot values from here on */
#define ESI_SAMPLES_RCVD 0x010
#define ESI_SEQ_ERRORS   0x014
#define ESI_MARKER_ERRORS 0x018
#define ESI_MULTI_ERRORS 0x01C
#define ESI_TX_CLK_COUNT 0x020
#define ESI_RX_CLK_COUNT 0x024
#define ESI_ACLK_COUNT   0x028
#define ESI_LINK_DROPS   0x02C
#define ESI_ID_WORD      0x45534931
#define ESI_ST_XCVR_READY (1u << 5)
#define ESI_ST_LINK_READY (1u << 7)
#define ESI_ST_SNAP_BUSY  (1u << 8)
#define ESI_CTL_SNAPSHOT  (1u << 7)
#define ESI_CTL_GT_RESET  (1u << 3)
#define ESI_CTL_KEEP_MASK 0x71u  /* link reset + loopback mode survive a control write */

#define FRAME_SOFTS      2144    /* soft bits per frame (sync + payload) = bytes per RX transfer */
#define RX_AHEAD         2       /* receive transfers kept outstanding (see the arming loop) */
#define RX_IDLE_MS       3000    /* dma-proxy's own wait timeout; one timed-out RX = end of run */

static int verbose = 0;
static unsigned rx_bytes = FRAME_SOFTS;   /* -L */

/* open the UIO device whose /sys/class/uio/uioN/name matches; -1 if none */
static int open_uio_by_name(const char *name)
{
    char path[64], buf[64];
    for (int n = 0; n < 16; n++) {
        snprintf(path, sizeof path, "/sys/class/uio/uio%d/name", n);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        buf[0] = 0;
        if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
        fclose(f);
        buf[strcspn(buf, "\n")] = 0;
        if (strcmp(buf, name) == 0) {
            snprintf(path, sizeof path, "/dev/uio%d", n);
            int fd = open(path, O_RDWR | O_SYNC);
            if (fd < 0) perror(path);
            else if (verbose) fprintf(stderr, "%s is %s\n", name, path);
            return fd;
        }
    }
    return -1;
}

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

/* ---- the ESIstream link (tier 2) ------------------------------------- */
static const char *esi_status_bits[] = {
    "gt_powergood", "lcpll_lock", "tx_resetdone", "rx_resetdone", "gt_link_status",
    "xcvr_ready", "lanes_ready", "link_ready", "snapshot_busy", "tx_clk_alive", "rx_clk_alive"
};

/* the link block's map0 when the tier-2 image is loaded, NULL on a tier-1 image */
static volatile uint32_t *open_esi_link(void)
{
    int fd = open_uio_by_name("esi_link_hw");
    if (fd < 0) return NULL;
    volatile uint32_t *esi = map_uio(fd, 0, 0x10000);
    if (!esi) return NULL;
    if (rd(esi, ESI_ID) != ESI_ID_WORD) {
        fprintf(stderr, "warning: esi_link_hw ID 0x%08X is not the ESIstream link (0x%08X); ignoring it\n",
                rd(esi, ESI_ID), ESI_ID_WORD);
        return NULL;
    }
    return esi;
}

/* latch the link-domain counters (they run at 156.25 MHz; a direct read could tear) */
static int esi_snapshot(volatile uint32_t *esi)
{
    wr(esi, ESI_CONTROL, (rd(esi, ESI_CONTROL) & ESI_CTL_KEEP_MASK) | ESI_CTL_SNAPSHOT);
    for (int i = 0; i < 1000; i++)
        if (!(rd(esi, ESI_STATUS) & ESI_ST_SNAP_BUSY)) return 0;
    fprintf(stderr, "ESIstream link: snapshot never completed -- a link clock is not running\n");
    return -1;
}

/* pulse the bridge's gtreset_in (8 PL clocks, stretched in the block), keep the loopback mode */
static void esi_gt_reset(volatile uint32_t *esi)
{
    wr(esi, ESI_CONTROL, (rd(esi, ESI_CONTROL) & ESI_CTL_KEEP_MASK) | ESI_CTL_GT_RESET);
    printf("transceiver reset pulsed; waiting 500 ms for the bridge's reset sequence\n");
    usleep(500000);
}

/* status line, counters, and (with the ratio) whether the GT reference clock is 156.25 MHz */
static int esi_report(volatile uint32_t *esi, int with_ratio)
{
    uint32_t st = rd(esi, ESI_STATUS), ctl = rd(esi, ESI_CONTROL);
    printf("ESI_STATUS 0x%08X  CONTROL 0x%08X  (loopback mode %u)\n", st, ctl, (ctl >> 4) & 7);
    printf("  ");
    for (unsigned b = 0; b < sizeof esi_status_bits / sizeof *esi_status_bits; b++)
        printf("%s=%u ", esi_status_bits[b], (st >> b) & 1);
    printf("\n");
    if (esi_snapshot(esi) < 0) return -1;
    uint32_t sent = rd(esi, ESI_SAMPLES_SENT), rcvd = rd(esi, ESI_SAMPLES_RCVD);
    uint32_t e1 = rd(esi, ESI_SEQ_ERRORS), e2 = rd(esi, ESI_MARKER_ERRORS), e3 = rd(esi, ESI_MULTI_ERRORS);
    printf("ESI_SAMPLES_SENT %u  ESI_SAMPLES_RCVD %u  %s\n", sent, rcvd, sent == rcvd ? "(equal)" : "(NOT EQUAL)");
    printf("ESI_SEQ_ERRORS %u  ESI_MARKER_ERRORS %u  ESI_MULTI_ERRORS %u  ESI_LINK_DROPS %u\n",
           e1, e2, e3, rd(esi, ESI_LINK_DROPS));
    if (with_ratio) {
        uint32_t t1 = rd(esi, ESI_TX_CLK_COUNT), a1 = rd(esi, ESI_ACLK_COUNT);
        usleep(200000);
        if (esi_snapshot(esi) < 0) return -1;
        uint32_t dt = rd(esi, ESI_TX_CLK_COUNT) - t1, da = rd(esi, ESI_ACLK_COUNT) - a1;
        printf("tx_clk / PL clock over %u PL clocks = %.4f  (156.25 MHz / 100 MHz = 1.5625; 1.0000 = SI570 still at 100 MHz)\n",
               da, da ? (double)dt / (double)da : 0.0);
    }
    return (st & ESI_ST_LINK_READY) ? 0 : 1;
}

static int usage(const char *a)
{
    fprintf(stderr, "usage: %s -i stim.bin -o soft.bin [-f max_frames] [-L rx_bytes] [-q] [-v]\n"
                    "       %s -l            (tier 2: ESIstream link status and counters only)\n"
                    "       %s -r            (tier 2: pulse the transceiver reset, then as -l)\n", a, a, a);
    return 1;
}

int main(int argc, char **argv)
{
    const char *in_path = NULL, *out_path = NULL;
    int max_frames = 0, want_readbacks = 0, link_only = 0, gt_reset = 0, opt;
    while ((opt = getopt(argc, argv, "i:o:f:L:lqrv")) != -1) {
        switch (opt) {
        case 'i': in_path = optarg; break;
        case 'o': out_path = optarg; break;
        case 'f': max_frames = atoi(optarg); break;
        case 'L': rx_bytes = (unsigned)strtoul(optarg, NULL, 0); break;
        case 'l': link_only = 1; break;
        case 'r': link_only = 1; gt_reset = 1; break;
        case 'q': want_readbacks = 1; break;
        case 'v': verbose = 1; break;
        default: return usage(argv[0]);
        }
    }
    if (link_only) {
        volatile uint32_t *esi = open_esi_link();
        if (!esi) { fprintf(stderr, "no UIO device named esi_link_hw: tier-1 image?\n"); return 1; }
        if (gt_reset) esi_gt_reset(esi);
        return esi_report(esi, 1) == 0 ? 0 : 1;
    }
    if (!in_path || !out_path) return usage(argv[0]);
    if (rx_bytes == 0 || rx_bytes > BUFFER_SIZE) {
        fprintf(stderr, "-L must be 1..%u\n", (unsigned)BUFFER_SIZE); return 1;
    }

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
    int uio = open_uio_by_name("haifuraiya_rx_axi");
    if (uio < 0) { fprintf(stderr, "no UIO device named haifuraiya_rx_axi (ls /sys/class/uio/*/name)\n"); return 1; }
    volatile uint32_t *ch = map_uio(uio, 0, 0x10000);
    volatile uint32_t *dm = map_uio(uio, 1, 0x10000);
    if (!ch || !dm) return 1;
    if (rd(ch, CH_VERSION) != 0x00010000)
        fprintf(stderr, "warning: channelizer VERSION 0x%08X (expected 0x00010000)\n", rd(ch, CH_VERSION));
    /* tier 2: the samples go through the link; it must be up before we queue any */
    volatile uint32_t *esi = open_esi_link();
    if (esi) {
        uint32_t st = rd(esi, ESI_STATUS);
        fprintf(stderr, "ESIstream link present (tier 2): STATUS 0x%08X\n", st);
        if (!(st & ESI_ST_LINK_READY)) {
            fprintf(stderr, "ESIstream link is not ready (xcvr_ready=%u link_ready=0); see opv_play -l\n",
                    (st & ESI_ST_XCVR_READY) ? 1u : 0u);
            return 1;
        }
    }

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

    /* Arm only RX_AHEAD receive transfers, not all BUFFER_COUNT.  The Xilinx
     * DMA driver sets the interrupt-coalescing threshold to the number of
     * descriptors it starts as a batch: with 32 armed up front, frame 1
     * completed alone (threshold 1) and then the remaining 31 were started as
     * one batch -- no interrupt until 31 frames landed, and a run has 24.
     * First hardware run with the DMA fix, 2026-10-25: exactly one frame
     * delivered, the rest in DDR with nobody told.  Two outstanding (one
     * active, one queued) keeps every batch at size 1; the receiver's
     * 121-frame soft-bit FIFO covers the re-arm latency many times over. */
    for (int b = 0; b < RX_AHEAD; b++) {
        rx[b].length = rx_bytes;
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
            fwrite(rx[rx_id].buffer, 1, rx_bytes, out);
            frames++;
            if (verbose) fprintf(stderr, "  frame %d (rx buffer %d)\n", frames, rx_id);
            /* re-arm RX_AHEAD buffers ahead of the one just drained */
            int next = (rx_id + RX_AHEAD) % BUFFER_COUNT;
            rx[next].length = rx_bytes;
            if (ioctl(rx_fd, START_XFER, &next) < 0) { perror("START_XFER rx re-arm"); return 2; }
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
            frames, frames * (int)rx_bytes, secs, RX_IDLE_MS);
    if (want_readbacks) {
        readbacks(ch, dm);
        if (esi) {
            esi_report(esi, 0);
            printf("tier 2 pass: the tier-1 grade, and SAMPLES_SENT = SAMPLES_RCVD = %zu, all error counters 0\n",
                   in_bytes / 4);
        }
    }
    return 0;
}
