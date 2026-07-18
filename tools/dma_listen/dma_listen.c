/* dma_listen.c — Listen to the demod soft-bit DMA stream on ZCU102
 *
 * Opens a libiio context, creates a buffer on axi-adrv9002-rx-lpc, loops
 * on iio_buffer_refill(), and prints per-refill statistics. Optionally
 * writes the raw bytes to a file for opv-decode.
 *
 * SEAM (read this before trusting the numbers):
 * Since the Phase B splice (syn/zcu102_with_adrv9001/system_bd.tcl, B.9),
 * axi_adrv9001_rx1_dma is fed by the DEMODULATOR'S SOFT-BIT OUTPUT, not
 * raw ADC samples and not channel IQ:
 *
 *     channelizer_rx1/m_axis_soft_bit -> soft_widen_rx1 -> rx1_dma
 *
 * Each beat is one 3-bit soft value (0..7) zero-extended to a byte —
 * exactly the layout `opv-decode -3` consumes. The stream is BURSTY:
 * soft bits are emitted only for decoded frame payloads. One frame is
 *
 *     40 ms x 54,200 baud = 2168 symbols, minus the 24-bit sync word
 *     consumed by the frame-sync detector = 2144 soft bytes per frame
 *
 * (hardware-verified: PARTIAL_XFER_LEN read 0x10C0 = 4288 = 2 frames
 * after exactly 2 frame decodes). While frame-locked at 25 frames/s the
 * stream runs ~53.6 kB/s; on a quiet band it is SILENT and refills
 * simply do not complete. A refill timeout here usually means "no
 * frames," not "broken plumbing."
 *
 * The IIO device still describes itself as 2x int16 IQ at 20 Msps —
 * that is the ADC driver's vestigial self-image, not the stream. This
 * tool walks the buffer as raw bytes and ignores the channel format.
 *
 * DIAGNOSTIC VALUE: any byte > 7 is impossible from the 3-bit widener —
 * the out-of-range counter is a plumbing-corruption detector. The soft
 * histogram is also the hardware-measured soft distribution needed for
 * fsync quantizer calibration (compare against opv-decode -3's law).
 *
 * ROLE: PS<->PL diagnostic and payload tap. The soft bytes traverse
 * ADC -> SSI -> halfband -> channelizer -> eq -> normalizer -> MLSE
 * demod -> frame sync -> widener -> DMAC -> DDR, so bytes landing here
 * vouch for the entire RX fabric path end to end.
 *
 * Build:
 *   aarch64-linux-gnu-gcc -O2 -Wall -o dma_listen dma_listen.c -liio -lm
 *
 * Or for native build on the target:
 *   gcc -O2 -Wall -o dma_listen dma_listen.c -liio -lm
 *
 * Usage:
 *   ./dma_listen                       # local context, stats only
 *   ./dma_listen ip:10.73.1.16         # network libiio context
 *   ./dma_listen -n 536                # 536 samples = 2144 B = 1 frame/refill
 *   ./dma_listen -c 100                # stop after 100 refills
 *   ./dma_listen -t 5000 -k            # 5 s timeout, retry forever (SIGINT-able)
 *   ./dma_listen -t 5000 -k -w /tmp/softbits.bin   # first-light capture
 *
 * FIRST-LIGHT IDIOM: -t 5000 -k -w <file>. The tool rides through quiet
 * timeouts, rings one line per timeout, and captures every frame's soft
 * bytes the moment the band comes alive. Then:  opv-decode -3 <file>.
 * (Avoid -t 0: a refill blocked with no timeout is uninterruptible in
 * the kernel and shrugs off Ctrl-C; you will need kill -9.)
 *
 * License: CERN-OHL-S v2 (matches the rest of Haifuraiya)
 */

#include <iio.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ----------------------------------------------------------------------- */
/* Configuration                                                            */
/* ----------------------------------------------------------------------- */

#define DEFAULT_BUFFER_SAMPLES  4096        /* IIO samples (4 B each) */
#define DEFAULT_RX_DEV          "axi-adrv9002-rx-lpc"

/* 40 ms x 54,200 baud = 2168 symbols; minus 24-bit sync word = 2144
 * payload soft bits per frame, one byte each after the widener. */
#define FRAME_SOFT_BYTES        2144

static volatile sig_atomic_t stop_requested = 0;

static void on_sigint(int sig)
{
    (void)sig;
    stop_requested = 1;
}

/* ----------------------------------------------------------------------- */
/* Soft-bit statistics                                                      */
/* ----------------------------------------------------------------------- */

struct stats {
    size_t   total_bytes;
    uint64_t hist[8];       /* histogram of in-range soft values 0..7 */
    uint64_t sum;           /* sum of in-range soft values            */
    size_t   oor_count;     /* bytes > 7: impossible from the widener */
    uint8_t  oor_example;   /* first offending byte, for the log      */
};

static void stats_reset(struct stats *s)
{
    memset(s, 0, sizeof(*s));
}

static void stats_update(struct stats *s, const uint8_t *p, size_t n)
{
    s->total_bytes += n;
    for (size_t i = 0; i < n; i++) {
        uint8_t v = p[i];
        if (v < 8) {
            s->hist[v]++;
            s->sum += v;
        } else {
            if (s->oor_count == 0)
                s->oor_example = v;
            s->oor_count++;
        }
    }
}

static void stats_print(const struct stats *s, double elapsed_sec, FILE *out)
{
    if (s->total_bytes == 0) {
        fprintf(out, "no bytes in this refill\n");
        return;
    }

    size_t in_range = s->total_bytes - s->oor_count;
    double mean = in_range ? (double)s->sum / in_range : 0.0;
    double frames = (double)s->total_bytes / FRAME_SOFT_BYTES;

    fprintf(out,
        "[%.3fs] %zu B = %.2f frames | soft mean=%.2f | "
        "hist 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu 5:%llu 6:%llu 7:%llu",
        elapsed_sec, s->total_bytes, frames, mean,
        (unsigned long long)s->hist[0], (unsigned long long)s->hist[1],
        (unsigned long long)s->hist[2], (unsigned long long)s->hist[3],
        (unsigned long long)s->hist[4], (unsigned long long)s->hist[5],
        (unsigned long long)s->hist[6], (unsigned long long)s->hist[7]);

    if (s->oor_count)
        fprintf(out, " | OOR=%zu (first=0x%02X) *** CORRUPTION ***",
                s->oor_count, s->oor_example);

    /* Non-integer frame counts are expected only if a refill filled to
     * the buffer limit mid-frame; partial transfers normally end refills
     * on frame (tlast) boundaries. */
    double frac = frames - (long)frames;
    if (frac > 0.001 && frac < 0.999)
        fprintf(out, " | non-integer frames (mid-frame boundary)");

    fprintf(out, "\n");
}

/* ----------------------------------------------------------------------- */
/* Main                                                                     */
/* ----------------------------------------------------------------------- */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Listen to the demod soft-bit DMA stream (rx1) and report per-refill\n"
        "statistics; optionally capture raw bytes for opv-decode -3.\n"
        "\n"
        "usage: %s [options] [uri]\n"
        "  -n <samples>   buffer size in IIO samples of 4 B (default: %d;\n"
        "                 536 samples = 2144 B = exactly one frame)\n"
        "  -c <count>     stop after this many refills (default: unlimited)\n"
        "  -d <dev>       device name (default: %s)\n"
        "  -t <ms>        refill timeout in ms; 0 = no timeout (block forever,\n"
        "                 NOT Ctrl-C-able while blocked — prefer -t 5000 -k)\n"
        "  -k             keep retrying on refill timeout instead of exiting\n"
        "  -w <file>      write raw soft bytes to <file> (opv-decode -3 layout)\n"
        "  uri            libiio context uri, e.g. ip:10.73.1.16 (default: local)\n",
        prog, DEFAULT_BUFFER_SAMPLES, DEFAULT_RX_DEV);
}

int main(int argc, char **argv)
{
    /* Defaults */
    size_t buffer_samples = DEFAULT_BUFFER_SAMPLES;
    long refill_count = 0;      /* 0 = unlimited */
    long timeout_ms = -1;       /* -1 = library default; 0 = no timeout */
    bool keep_going = false;    /* retry (not exit) on refill timeout */
    const char *device_name = DEFAULT_RX_DEV;
    const char *capture_path = NULL;
    const char *uri = NULL;

    /* Parse args */
    int opt;
    while ((opt = getopt(argc, argv, "n:c:d:t:w:kh")) != -1) {
        switch (opt) {
            case 'n': buffer_samples = (size_t)atol(optarg); break;
            case 'c': refill_count = atol(optarg); break;
            case 'd': device_name = optarg; break;
            case 't': timeout_ms = atol(optarg); break;
            case 'w': capture_path = optarg; break;
            case 'k': keep_going = true; break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }
    if (optind < argc) uri = argv[optind];

    if (buffer_samples < 64) {
        fprintf(stderr, "buffer_samples must be >= 64\n");
        return 1;
    }

    /* Set up Ctrl-C handler */
    signal(SIGINT, on_sigint);

    /* Optional capture file */
    FILE *capture = NULL;
    size_t capture_bytes = 0;
    if (capture_path) {
        capture = fopen(capture_path, "wb");
        if (!capture) {
            fprintf(stderr, "failed to open capture file '%s'\n", capture_path);
            return 1;
        }
        fprintf(stderr, "capture: writing raw soft bytes to %s\n", capture_path);
    }

    /* Create libiio context */
    struct iio_context *ctx;
    if (uri) {
        ctx = iio_create_context_from_uri(uri);
        if (!ctx) {
            fprintf(stderr, "failed to create context from uri '%s'\n", uri);
            if (capture) fclose(capture);
            return 1;
        }
        fprintf(stderr, "libiio: connected to %s\n", uri);
    } else {
        ctx = iio_create_default_context();
        if (!ctx) {
            fprintf(stderr, "failed to create default libiio context\n");
            if (capture) fclose(capture);
            return 1;
        }
        fprintf(stderr, "libiio: using default (local) context\n");
    }

    fprintf(stderr, "libiio: context has %u devices\n",
            iio_context_get_devices_count(ctx));

    /* Optional refill timeout override. 0 disables the timeout entirely
     * (refill blocks until data arrives; uninterruptible while blocked). */
    if (timeout_ms >= 0) {
        int terr = iio_context_set_timeout(ctx, (unsigned int)timeout_ms);
        if (terr != 0)
            fprintf(stderr, "warning: iio_context_set_timeout(%ld) failed (%d)\n",
                    timeout_ms, terr);
        else
            fprintf(stderr, "libiio: refill timeout %s\n",
                    timeout_ms == 0 ? "disabled (block forever)" : "set");
    }

    /* Find the RX device */
    struct iio_device *rx_dev = iio_context_find_device(ctx, device_name);
    if (!rx_dev) {
        fprintf(stderr, "device '%s' not found in context\n", device_name);
        iio_context_destroy(ctx);
        if (capture) fclose(capture);
        return 1;
    }

    /* Enable the device's channels so buffer creation succeeds. The
     * channel format (2x int16 "IQ") is the ADC driver's vestigial
     * self-image; the payload is soft-bit bytes and we walk it raw. */
    struct iio_channel *rx_ch_i = iio_device_find_channel(rx_dev, "voltage0_i", false);
    struct iio_channel *rx_ch_q = iio_device_find_channel(rx_dev, "voltage0_q", false);
    if (!rx_ch_i || !rx_ch_q) {
        fprintf(stderr, "could not find voltage0_i (I) or voltage0_q (Q) channel on %s\n",
                device_name);
        iio_context_destroy(ctx);
        if (capture) fclose(capture);
        return 1;
    }
    iio_channel_enable(rx_ch_i);
    iio_channel_enable(rx_ch_q);

    /* Create the buffer */
    struct iio_buffer *buf = iio_device_create_buffer(rx_dev, buffer_samples, false);
    if (!buf) {
        fprintf(stderr, "failed to create buffer of %zu samples\n", buffer_samples);
        iio_context_destroy(ctx);
        if (capture) fclose(capture);
        return 1;
    }

    size_t buffer_bytes = buffer_samples * 4;
    fprintf(stderr,
        "libiio: buffer %zu samples = %zu B (%.2f frames at %d soft B/frame)\n"
        "stream: demod 3-bit soft bits, 1 byte each; bursty — ~%.1f kB/s while\n"
        "stream: frame-locked at 25 fps, silent otherwise (timeouts = no frames)\n",
        buffer_samples, buffer_bytes,
        (double)buffer_bytes / FRAME_SOFT_BYTES, FRAME_SOFT_BYTES,
        FRAME_SOFT_BYTES * 25.0 / 1000.0);

    /* Main refill loop */
    long refills_done = 0;
    struct stats s;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    fprintf(stderr, "\n--- starting refill loop (Ctrl-C to stop) ---\n");

    while (!stop_requested) {
        ssize_t nbytes = iio_buffer_refill(buf);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

        if (nbytes < 0) {
            char errbuf[128];
            iio_strerror((int)-nbytes, errbuf, sizeof(errbuf));
            if (keep_going && (int)-nbytes == ETIMEDOUT) {
                fprintf(stderr, "[%.3fs] refill timeout — no frames yet, "
                        "retrying (-k)\n", elapsed);
                continue;
            }
            fprintf(stderr, "iio_buffer_refill failed: %s\n", errbuf);
            break;
        }

        /* Walk the buffer as raw soft-bit bytes. */
        const uint8_t *p = (const uint8_t *)iio_buffer_start(buf);

        stats_reset(&s);
        stats_update(&s, p, (size_t)nbytes);
        stats_print(&s, elapsed, stderr);

        if (capture && nbytes > 0) {
            size_t wr = fwrite(p, 1, (size_t)nbytes, capture);
            capture_bytes += wr;
            if (wr != (size_t)nbytes) {
                fprintf(stderr, "capture: short write (%zu of %zu B) — stopping\n",
                        wr, (size_t)nbytes);
                break;
            }
            fflush(capture);
        }

        refills_done++;
        if (refill_count > 0 && refills_done >= refill_count) {
            fprintf(stderr, "reached %ld refills, exiting\n", refill_count);
            break;
        }
    }

    fprintf(stderr, "\n--- shutting down ---\n");
    fprintf(stderr, "total refills: %ld\n", refills_done);
    if (capture) {
        fclose(capture);
        fprintf(stderr, "capture: %zu B written to %s (%.2f frames)\n",
                capture_bytes, capture_path,
                (double)capture_bytes / FRAME_SOFT_BYTES);
    }

    iio_buffer_destroy(buf);
    iio_context_destroy(ctx);
    return 0;
}
