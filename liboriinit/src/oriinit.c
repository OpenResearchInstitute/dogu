/* SPDX-License-Identifier: CERN-OHL-S-2.0 */
/*
 * liboriinit — ADRV9002 chip-init library for ORI SDR builds.
 *
 * Implementation. See ../include/oriinit.h for the public API.
 *
 * Copyright (C) 2026 Open Research Institute and contributors.
 */

#include "oriinit.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <iio.h>

/*------------------------------------------------------------------*/
/* Internal context                                                 */
/*------------------------------------------------------------------*/

/* The ADRV9002 PHY device exposes 4 channels:
 *   - "voltage0" input  → RX1
 *   - "voltage1" input  → RX2
 *   - "voltage0" output → TX1
 *   - "voltage1" output → TX2
 */
struct oriinit_ctx {
    struct iio_context *iio;
    struct iio_device  *phy;        /* adrv9002-phy */
    struct iio_channel *ch[ORIINIT_CH_MAX];
};

/* PHY device name in libiio. */
#define PHY_DEVICE_NAME "adrv9002-phy"

/* Mapping from oriinit_channel_t to libiio channel name + direction. */
struct channel_descriptor {
    const char *name;
    bool        output;
};

static const struct channel_descriptor CHANNEL_DESCRIPTORS[ORIINIT_CH_MAX] = {
    [ORIINIT_CH_RX1] = { "voltage0", false },
    [ORIINIT_CH_RX2] = { "voltage1", false },
    [ORIINIT_CH_TX1] = { "voltage0", true  },
    [ORIINIT_CH_TX2] = { "voltage1", true  },
};

/*------------------------------------------------------------------*/
/* ENSM mode string ↔ enum                                          */
/*------------------------------------------------------------------*/

static oriinit_ensm_t parse_ensm_str(const char *s)
{
    if (!s) return ORIINIT_ENSM_UNKNOWN;
    if (strncmp(s, "rf_enabled", 10) == 0)  return ORIINIT_ENSM_RF_ENABLED;
    if (strncmp(s, "calibrated", 10) == 0)  return ORIINIT_ENSM_CALIBRATED;
    if (strncmp(s, "primed",      6) == 0)  return ORIINIT_ENSM_PRIMED;
    return ORIINIT_ENSM_UNKNOWN;
}

static const char *ensm_to_str(oriinit_ensm_t e)
{
    switch (e) {
        case ORIINIT_ENSM_RF_ENABLED: return "rf_enabled";
        case ORIINIT_ENSM_CALIBRATED: return "calibrated";
        case ORIINIT_ENSM_PRIMED:     return "primed";
        case ORIINIT_ENSM_UNKNOWN:
        default:                      return NULL;  /* invalid; do not write */
    }
}

/*------------------------------------------------------------------*/
/* Connection management                                            */
/*------------------------------------------------------------------*/

oriinit_ctx_t *oriinit_create(const char *iio_uri)
{
    oriinit_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    if (iio_uri && iio_uri[0]) {
        ctx->iio = iio_create_context_from_uri(iio_uri);
    } else {
        ctx->iio = iio_create_default_context();
    }
    if (!ctx->iio) {
        free(ctx);
        return NULL;
    }

    ctx->phy = iio_context_find_device(ctx->iio, PHY_DEVICE_NAME);
    if (!ctx->phy) {
        iio_context_destroy(ctx->iio);
        free(ctx);
        return NULL;
    }

    /* Look up all four channels. Missing channels are tolerated (some
     * boards might not have TX2 enabled, for example) — but RX1 must
     * exist for the library to be useful. */
    for (int i = 0; i < ORIINIT_CH_MAX; i++) {
        ctx->ch[i] = iio_device_find_channel(ctx->phy,
                                             CHANNEL_DESCRIPTORS[i].name,
                                             CHANNEL_DESCRIPTORS[i].output);
    }
    if (!ctx->ch[ORIINIT_CH_RX1]) {
        iio_context_destroy(ctx->iio);
        free(ctx);
        return NULL;
    }

    return ctx;
}

void oriinit_destroy(oriinit_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->iio) iio_context_destroy(ctx->iio);
    free(ctx);
}

/*------------------------------------------------------------------*/
/* Read state                                                       */
/*------------------------------------------------------------------*/

/* Helper: read a channel attribute as a string into buf. */
static int read_chan_attr_string(struct iio_channel *ch, const char *attr,
                                  char *buf, size_t bufsize)
{
    if (!ch) { buf[0] = '\0'; return -ENOENT; }
    ssize_t n = iio_channel_attr_read(ch, attr, buf, bufsize);
    if (n < 0) { buf[0] = '\0'; return (int)n; }
    return 0;
}

/* Helper: read a channel attribute as a long long. */
static int read_chan_attr_longlong(struct iio_channel *ch, const char *attr,
                                    long long *out)
{
    if (!ch) { *out = 0; return -ENOENT; }
    return (int)iio_channel_attr_read_longlong(ch, attr, out);
}

/* Helper: read a channel attribute as a bool ("1" / "0"). */
static int read_chan_attr_bool(struct iio_channel *ch, const char *attr,
                                bool *out)
{
    long long v;
    int rc = read_chan_attr_longlong(ch, attr, &v);
    if (rc < 0) { *out = false; return rc; }
    *out = (v != 0);
    return 0;
}

oriinit_status_t oriinit_read_state(oriinit_ctx_t *ctx, oriinit_state_t *out)
{
    if (!ctx || !out) return ORIINIT_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    char buf[64];
    long long ll;
    bool b;

    /* Per-channel reads. Skip channels we don't have a handle for. */
    for (int i = 0; i < ORIINIT_CH_MAX; i++) {
        struct iio_channel *ch = ctx->ch[i];
        if (!ch) continue;

        if (read_chan_attr_string(ch, "ensm_mode", buf, sizeof(buf)) == 0) {
            out->ensm[i] = parse_ensm_str(buf);
        }

        if (read_chan_attr_bool(ch, "en", &b) == 0) {
            out->enabled[i] = b;
        }

        /* "hardwaregain" returns something like "12 dB" — parse leading number.
         * iio_channel_attr_read_longlong silently fails on non-numeric leading
         * content, so use the string form and parse manually. */
        if (read_chan_attr_string(ch, "hardwaregain", buf, sizeof(buf)) == 0) {
            out->hardware_gain_db[i] = (int)strtol(buf, NULL, 10);
        }

        /* LO frequency (sometimes exposed as "RX_LO" / "TX_LO" via labeled
         * altvoltage channels rather than the voltage channels — TODO:
         * verify against actual hardware; for now try the voltage channel's
         * "lo_freq" or fallback). */
        if (read_chan_attr_longlong(ch, "lo_freq", &ll) == 0) {
            out->lo_frequency_hz[i] = ll;
        }
    }

    /* Device-level: sample rate.
     * Try device-level "sampling_frequency" first; that's the most reliable
     * value matching what flows out to the AXI-ADC bridge. */
    if (iio_device_attr_read_longlong(ctx->phy, "sampling_frequency", &ll) == 0) {
        out->sample_rate_hz = ll;
    } else if (ctx->ch[ORIINIT_CH_RX1]) {
        /* Fallback: per-channel attr on RX1. */
        if (read_chan_attr_longlong(ctx->ch[ORIINIT_CH_RX1],
                                    "sampling_frequency", &ll) == 0) {
            out->sample_rate_hz = ll;
        }
    }

    /* Device-level: initial_calibrations status. */
    if (iio_device_attr_read(ctx->phy, "initial_calibrations",
                              buf, sizeof(buf)) > 0) {
        out->initial_calibrations_running =
            (strncmp(buf, "run", 3) == 0);
    }

    return ORIINIT_OK;
}

/*------------------------------------------------------------------*/
/* Verify streaming                                                 */
/*------------------------------------------------------------------*/

oriinit_status_t oriinit_verify_streaming(oriinit_ctx_t *ctx,
                                          oriinit_channel_t ch)
{
    if (!ctx || ch >= ORIINIT_CH_MAX) return ORIINIT_ERR_INVALID_ARG;
    if (!ctx->ch[ch]) return ORIINIT_ERR_NO_CHANNEL;

    oriinit_state_t state;
    oriinit_status_t rc = oriinit_read_state(ctx, &state);
    if (rc != ORIINIT_OK) return rc;

    if (!state.enabled[ch])                      return ORIINIT_ERR_NOT_STREAMING;
    if (state.ensm[ch] != ORIINIT_ENSM_RF_ENABLED) return ORIINIT_ERR_NOT_STREAMING;
    return ORIINIT_OK;
}

/*------------------------------------------------------------------*/
/* Set ENSM                                                         */
/*------------------------------------------------------------------*/

oriinit_status_t oriinit_set_ensm(oriinit_ctx_t *ctx,
                                  oriinit_channel_t ch,
                                  oriinit_ensm_t state)
{
    if (!ctx || ch >= ORIINIT_CH_MAX) return ORIINIT_ERR_INVALID_ARG;
    if (!ctx->ch[ch]) return ORIINIT_ERR_NO_CHANNEL;

    const char *str = ensm_to_str(state);
    if (!str) return ORIINIT_ERR_INVALID_ARG;

    ssize_t rc = iio_channel_attr_write(ctx->ch[ch], "ensm_mode", str);
    if (rc < 0) return ORIINIT_ERR_IO;

    return ORIINIT_OK;
}

/*------------------------------------------------------------------*/
/* Run calibrations (safe sequence)                                 */
/*------------------------------------------------------------------*/

oriinit_status_t oriinit_run_calibrations(oriinit_ctx_t *ctx,
                                          unsigned int timeout_ms)
{
    if (!ctx) return ORIINIT_ERR_INVALID_ARG;

    /* 1. Snapshot current state so we can restore at the end. */
    oriinit_state_t before;
    oriinit_status_t rc = oriinit_read_state(ctx, &before);
    if (rc != ORIINIT_OK) return rc;

    /* 2. Drop RX1 and RX2 to CALIBRATED.
     *
     * THIS IS CRITICAL. The driver's behavior of wedging the AXI-ADC bridge
     * happens specifically when initial_calibrations=run is invoked while
     * an RX channel is in rf_enabled. We refuse to do that.
     */
    if (ctx->ch[ORIINIT_CH_RX1]) {
        rc = oriinit_set_ensm(ctx, ORIINIT_CH_RX1, ORIINIT_ENSM_CALIBRATED);
        if (rc != ORIINIT_OK) return rc;
    }
    if (ctx->ch[ORIINIT_CH_RX2]) {
        rc = oriinit_set_ensm(ctx, ORIINIT_CH_RX2, ORIINIT_ENSM_CALIBRATED);
        if (rc != ORIINIT_OK) return rc;
    }

    /* 3. Trigger calibrations. */
    ssize_t io = iio_device_attr_write(ctx->phy, "initial_calibrations", "run");
    if (io < 0) return ORIINIT_ERR_IO;

    /* 4. Poll initial_calibrations until it returns to "off" (or timeout). */
    struct timespec t0, t_now;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    char buf[64];
    bool finished = false;
    while (!finished) {
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        long elapsed_ms = (t_now.tv_sec - t0.tv_sec) * 1000
                        + (t_now.tv_nsec - t0.tv_nsec) / 1000000;
        if (elapsed_ms > (long)timeout_ms) {
            return ORIINIT_ERR_TIMEOUT;
        }

        if (iio_device_attr_read(ctx->phy, "initial_calibrations",
                                  buf, sizeof(buf)) > 0) {
            if (strncmp(buf, "off", 3) == 0) {
                finished = true;
                break;
            }
        }

        /* Sleep 50ms between polls — calibrations typically take 1-3 sec. */
        struct timespec sleep_ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&sleep_ts, NULL);
    }

    /* 5. Restore the saved ENSM states. */
    for (int i = 0; i < ORIINIT_CH_MAX; i++) {
        if (!ctx->ch[i]) continue;
        if (before.ensm[i] == ORIINIT_ENSM_UNKNOWN) continue;
        if (before.ensm[i] == ORIINIT_ENSM_CALIBRATED) continue;  /* already there */

        oriinit_status_t rs = oriinit_set_ensm(ctx, (oriinit_channel_t)i,
                                                before.ensm[i]);
        if (rs != ORIINIT_OK) {
            /* Best-effort restore; report the first error but don't stop
             * attempting the others. */
            rc = rs;
        }
    }

    return rc;
}

/*------------------------------------------------------------------*/
/* Load profile (stubbed pending working JSON)                      */
/*------------------------------------------------------------------*/

oriinit_status_t oriinit_load_profile(oriinit_ctx_t *ctx, const char *json_path)
{
    (void)ctx;
    (void)json_path;

    /* TODO: implement when we have a working TES-exported profile JSON
     * to test against. The implementation will:
     *   1. Read the JSON file into memory
     *   2. Drop both RX channels to CALIBRATED (precondition)
     *   3. Write the JSON to /sys/bus/iio/devices/iio:device1/profile_config
     *      via iio_device_attr_write_raw()
     *   4. Verify the load succeeded by reading profile_config back
     *   5. Restore ENSM states (or set to RF_ENABLED, depending on caller intent)
     *
     * We don't implement this yet because:
     *   - We don't have a known-working JSON for our specific kernel API
     *     version (68.5.0, requiring TES 0.23.1 which isn't available)
     *   - Implementing without ability to test against the hardware risks
     *     silent bugs in the file-handling / sysfs interaction
     *
     * When TES 0.23.1 is obtained (or a different profile path opens up),
     * this returns to the top of the priority queue.
     */

    return ORIINIT_ERR_NOT_IMPLEMENTED;
}

/*------------------------------------------------------------------*/
/* String helpers                                                   */
/*------------------------------------------------------------------*/

const char *oriinit_status_str(oriinit_status_t status)
{
    switch (status) {
        case ORIINIT_OK:                    return "OK";
        case ORIINIT_ERR_NO_CONTEXT:        return "libiio context creation failed";
        case ORIINIT_ERR_NO_DEVICE:         return "ADRV9002 PHY device not found";
        case ORIINIT_ERR_NO_CHANNEL:        return "requested channel not present";
        case ORIINIT_ERR_IO:                return "libiio read/write failure";
        case ORIINIT_ERR_INVALID_ARG:       return "invalid argument";
        case ORIINIT_ERR_INVALID_STATE:     return "chip in unexpected state";
        case ORIINIT_ERR_DANGEROUS_OP:      return "operation refused (don't-touch rule)";
        case ORIINIT_ERR_PROFILE_REJECTED:  return "driver rejected profile JSON";
        case ORIINIT_ERR_NOT_STREAMING:     return "channel not in streaming state";
        case ORIINIT_ERR_TIMEOUT:           return "operation timed out";
        case ORIINIT_ERR_NOT_IMPLEMENTED:   return "not yet implemented";
        default:                            return "unknown status";
    }
}

const char *oriinit_ensm_str(oriinit_ensm_t state)
{
    switch (state) {
        case ORIINIT_ENSM_RF_ENABLED: return "rf_enabled";
        case ORIINIT_ENSM_CALIBRATED: return "calibrated";
        case ORIINIT_ENSM_PRIMED:     return "primed";
        case ORIINIT_ENSM_UNKNOWN:
        default:                      return "unknown";
    }
}

const char *oriinit_channel_str(oriinit_channel_t ch)
{
    switch (ch) {
        case ORIINIT_CH_RX1: return "RX1";
        case ORIINIT_CH_RX2: return "RX2";
        case ORIINIT_CH_TX1: return "TX1";
        case ORIINIT_CH_TX2: return "TX2";
        default:             return "(invalid)";
    }
}

void oriinit_print_state(const oriinit_state_t *state, void *file_ptr)
{
    if (!state) return;
    FILE *f = file_ptr ? (FILE *)file_ptr : stderr;

    fprintf(f, "sample_rate_hz: %lld\n", state->sample_rate_hz);
    fprintf(f, "initial_calibrations_running: %s\n",
            state->initial_calibrations_running ? "yes" : "no");

    for (int i = 0; i < ORIINIT_CH_MAX; i++) {
        fprintf(f, "%s_ensm: %s\n",
                oriinit_channel_str((oriinit_channel_t)i),
                oriinit_ensm_str(state->ensm[i]));
        fprintf(f, "%s_enabled: %s\n",
                oriinit_channel_str((oriinit_channel_t)i),
                state->enabled[i] ? "yes" : "no");
        fprintf(f, "%s_gain_db: %d\n",
                oriinit_channel_str((oriinit_channel_t)i),
                state->hardware_gain_db[i]);
        fprintf(f, "%s_lo_hz: %lld\n",
                oriinit_channel_str((oriinit_channel_t)i),
                state->lo_frequency_hz[i]);
    }
}

/*------------------------------------------------------------------*/
/* Version                                                          */
/*------------------------------------------------------------------*/

const char *oriinit_version(void)
{
    return "0.1.0";
}
