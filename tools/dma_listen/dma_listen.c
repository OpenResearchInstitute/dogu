/* dma_listen.c — Validate ARM userspace ↔ channelizer DMA path on ZCU102
 *
 * Opens an libiio context, creates a buffer on axi-adrv9002-rx-lpc,
 * loops on iio_buffer_refill(), and prints summary statistics per
 * refill: byte count, distinct sample values, magnitude stats.
 *
 * ROLE: This is a *PS↔PL plumbing validator*, not the skeleton of a
 * demod daemon. In the Haifuraiya transponder architecture, the demod
 * lives entirely in the PL — the ARM never sees per-sample I/Q data
 * in production. This tool exists purely as a diagnostic:
 *
 *   - Confirms the AXI-ADC bridge is functional (PS can DMA from PL)
 *   - Detects the default-profile 0x00/0xFF binary pattern signature
 *   - Validates a custom profile produces multi-bit data
 *   - Can be used to capture raw I/Q files for offline analysis with
 *     opv-cxx-demod or other tools
 *
 * Not part of any production service. Run it manually during bring-up,
 * profile development, or when debugging "is the chip actually streaming
 * useful samples?" questions.
 *
 * Output is one summary line per refill to stderr, suitable for piping
 * to a log file.
 *
 * Build:
 *   aarch64-linux-gnu-gcc -O2 -Wall -o dma_listen dma_listen.c -liio -lm
 *
 * Or for native build on the target:
 *   gcc -O2 -Wall -o dma_listen dma_listen.c -liio -lm
 *
 * Usage:
 *   ./dma_listen                            # local libiio context
 *   ./dma_listen ip:10.73.1.16              # network libiio context
 *   ./dma_listen -n 4096                    # buffer size in samples
 *   ./dma_listen -c 100                     # stop after 100 refills
 *
 * License: CERN-OHL-S v2 (matches the rest of Haifuraiya)
 */

#include <iio.h>
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

#define DEFAULT_BUFFER_SAMPLES  4096
#define DEFAULT_RX_DEV          "axi-adrv9002-rx-lpc"

static volatile sig_atomic_t stop_requested = 0;

static void on_sigint(int sig)
{
    (void)sig;
    stop_requested = 1;
}

/* ----------------------------------------------------------------------- */
/* Sample statistics                                                        */
/* ----------------------------------------------------------------------- */

struct stats {
    size_t total_samples;
    size_t total_bytes;
    int64_t sum_i;
    int64_t sum_q;
    int64_t sum_mag_sq;     /* I*I + Q*Q, accumulator */
    int16_t min_i, max_i;
    int16_t min_q, max_q;
    size_t saturated_count; /* I or Q exactly ±32767 or ±32768 */
    size_t binary_count;    /* sample where I and Q are both 0 OR both -1 */
    /* Track distinct I values (small histogram) */
    uint8_t i_histogram[256]; /* sampled high byte of I */
};

static void stats_reset(struct stats *s)
{
    memset(s, 0, sizeof(*s));
    s->min_i = s->min_q = INT16_MAX;
    s->max_i = s->max_q = INT16_MIN;
}

static void stats_update(struct stats *s, int16_t I, int16_t Q)
{
    s->total_samples++;
    s->sum_i += I;
    s->sum_q += Q;
    s->sum_mag_sq += (int64_t)I * I + (int64_t)Q * Q;

    if (I < s->min_i) s->min_i = I;
    if (I > s->max_i) s->max_i = I;
    if (Q < s->min_q) s->min_q = Q;
    if (Q > s->max_q) s->max_q = Q;

    if (I == 32767 || I == -32768 || Q == 32767 || Q == -32768)
        s->saturated_count++;

    /* "binary" pattern from the default profile: I and Q both 0x0000 or
     * 0xFFFF (which is int16 -1). This is the most common diagnostic
     * signature on this build. */
    if ((I == 0 || I == -1) && (Q == 0 || Q == -1))
        s->binary_count++;

    /* Histogram on the high byte of I, captures coarse value distribution
     * cheaply without a 65536-entry array. */
    uint8_t high_i = (uint8_t)((I >> 8) & 0xff);
    if (s->i_histogram[high_i] < 255)
        s->i_histogram[high_i]++;
}

static size_t stats_distinct_high_bytes(const struct stats *s)
{
    size_t n = 0;
    for (int i = 0; i < 256; i++)
        if (s->i_histogram[i])
            n++;
    return n;
}

static void stats_print(const struct stats *s, double elapsed_sec,
                        double refill_dur_sec, FILE *out)
{
    if (s->total_samples == 0) {
        fprintf(out, "no samples in this refill\n");
        return;
    }

    double mean_i = (double)s->sum_i / s->total_samples;
    double mean_q = (double)s->sum_q / s->total_samples;
    double rms = sqrt((double)s->sum_mag_sq / s->total_samples);
    double binary_frac = (double)s->binary_count / s->total_samples;
    double sat_frac = (double)s->saturated_count / s->total_samples;
    size_t distinct = stats_distinct_high_bytes(s);

    /* Per-refill throughput, not cumulative */
    double mb_per_sec = (refill_dur_sec > 0)
        ? (s->total_bytes / refill_dur_sec) / 1e6
        : 0.0;

    const char *pattern_hint =
        (binary_frac > 0.95) ? "DEFAULT-PROFILE BINARY (no real ADC)"
        : (distinct >= 20)   ? "MULTI-BIT (real ADC samples?)"
        :                      "OTHER (intermediate)";

    fprintf(out,
        "[%.3fs] %zu samples (%zu B, %.2f MB/s) | I[%d..%d] mean=%.1f | "
        "Q[%d..%d] mean=%.1f | RMS=%.1f | bin=%.1f%% sat=%.1f%% "
        "distinct_high_I=%zu | %s\n",
        elapsed_sec, s->total_samples, s->total_bytes,
        mb_per_sec,
        s->min_i, s->max_i, mean_i,
        s->min_q, s->max_q, mean_q,
        rms,
        100.0 * binary_frac,
        100.0 * sat_frac,
        distinct,
        pattern_hint);
}

/* ----------------------------------------------------------------------- */
/* Main                                                                     */
/* ----------------------------------------------------------------------- */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] [uri]\n"
        "\n"
        "Validate ARM↔channelizer DMA on ZCU102 by reading I/Q samples\n"
        "via libiio and printing summary stats per refill.\n"
        "\n"
        "Options:\n"
        "  -n <samples>   buffer size in samples (default: %d)\n"
        "  -c <count>     stop after N refills (default: run until Ctrl-C)\n"
        "  -d <dev>       device name (default: %s)\n"
        "  -h             this help\n"
        "\n"
        "uri               libiio context URI (default: local:)\n"
        "                  e.g. 'ip:10.73.1.16' to connect remotely\n"
        "\n",
        prog, DEFAULT_BUFFER_SAMPLES, DEFAULT_RX_DEV);
}

int main(int argc, char *argv[])
{
    /* Defaults */
    size_t buffer_samples = DEFAULT_BUFFER_SAMPLES;
    long refill_count = 0; /* 0 = unlimited */
    const char *device_name = DEFAULT_RX_DEV;
    const char *uri = NULL;

    /* Parse args */
    int opt;
    while ((opt = getopt(argc, argv, "n:c:d:h")) != -1) {
        switch (opt) {
            case 'n': buffer_samples = (size_t)atol(optarg); break;
            case 'c': refill_count = atol(optarg); break;
            case 'd': device_name = optarg; break;
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

    /* Create libiio context */
    struct iio_context *ctx;
    if (uri) {
        ctx = iio_create_context_from_uri(uri);
        if (!ctx) {
            fprintf(stderr, "failed to create context from uri '%s'\n", uri);
            return 1;
        }
        fprintf(stderr, "libiio: connected to %s\n", uri);
    } else {
        ctx = iio_create_default_context();
        if (!ctx) {
            fprintf(stderr, "failed to create default libiio context\n");
            return 1;
        }
        fprintf(stderr, "libiio: using default (local) context\n");
    }

    fprintf(stderr, "libiio: context has %u devices\n",
            iio_context_get_devices_count(ctx));

    /* Find the RX device */
    struct iio_device *rx_dev = iio_context_find_device(ctx, device_name);
    if (!rx_dev) {
        fprintf(stderr, "device '%s' not found in context\n", device_name);
        iio_context_destroy(ctx);
        return 1;
    }

    /* Find I and Q channels.
     * ADRV9002 uses voltage0_i / voltage0_q (not voltage0 / voltage1 like AD9361). */
    struct iio_channel *rx_ch_i = iio_device_find_channel(rx_dev, "voltage0_i", false);
    struct iio_channel *rx_ch_q = iio_device_find_channel(rx_dev, "voltage0_q", false);
    if (!rx_ch_i || !rx_ch_q) {
        fprintf(stderr, "could not find voltage0_i (I) or voltage0_q (Q) channel on %s\n",
                device_name);
        iio_context_destroy(ctx);
        return 1;
    }

    /* Enable both channels */
    iio_channel_enable(rx_ch_i);
    iio_channel_enable(rx_ch_q);

    /* Create the buffer */
    struct iio_buffer *buf = iio_device_create_buffer(rx_dev, buffer_samples, false);
    if (!buf) {
        fprintf(stderr, "failed to create buffer of %zu samples\n", buffer_samples);
        iio_context_destroy(ctx);
        return 1;
    }
    fprintf(stderr, "libiio: created buffer for %zu samples (~%zu bytes per refill)\n",
            buffer_samples, buffer_samples * 4);

    /* Print sample-rate info if available */
    long long fs_hz = 0;
    if (iio_device_attr_read_longlong(rx_dev, "in_voltage_sampling_frequency",
                                       &fs_hz) == 0) {
        double dur_per_refill = (double)buffer_samples / fs_hz;
        fprintf(stderr, "libiio: sample rate %.3f Msps → each refill covers %.2f ms\n",
                fs_hz / 1e6, dur_per_refill * 1000);
    }

    /* Main refill loop */
    long refills_done = 0;
    struct stats s;
    struct timespec t0, t1, t_refill_start;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    fprintf(stderr, "\n--- starting refill loop (Ctrl-C to stop) ---\n");

    while (!stop_requested) {
        clock_gettime(CLOCK_MONOTONIC, &t_refill_start);
        ssize_t nbytes = iio_buffer_refill(buf);
        clock_gettime(CLOCK_MONOTONIC, &t1);

        if (nbytes < 0) {
            char errbuf[128];
            iio_strerror((int)-nbytes, errbuf, sizeof(errbuf));
            fprintf(stderr, "iio_buffer_refill failed: %s\n", errbuf);
            break;
        }

        /* Walk the buffer extracting (I, Q) pairs.
         * AXI-ADC delivers 4 bytes per sample: int16 I, int16 Q (little-endian) */
        stats_reset(&s);
        s.total_bytes = (size_t)nbytes;

        char *p_start = iio_buffer_first(buf, rx_ch_i);
        char *p_end   = iio_buffer_end(buf);
        ptrdiff_t step = iio_buffer_step(buf);

        for (char *p = p_start; p < p_end; p += step) {
            int16_t I = ((int16_t *)p)[0];
            int16_t Q = ((int16_t *)p)[1];
            stats_update(&s, I, Q);
        }

        double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        double refill_dur = (t1.tv_sec - t_refill_start.tv_sec)
                          + (t1.tv_nsec - t_refill_start.tv_nsec) / 1e9;

        stats_print(&s, elapsed, refill_dur, stderr);

        refills_done++;
        if (refill_count > 0 && refills_done >= refill_count) {
            fprintf(stderr, "reached %ld refills, exiting\n", refill_count);
            break;
        }
    }

    fprintf(stderr, "\n--- shutting down ---\n");
    fprintf(stderr, "total refills: %ld\n", refills_done);

    iio_buffer_destroy(buf);
    iio_context_destroy(ctx);
    return 0;
}
