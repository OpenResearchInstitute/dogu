/* SPDX-License-Identifier: CERN-OHL-S-2.0 */
/*
 * liboriinit — ADRV9002 chip-init library for ORI SDR builds.
 *
 * Encapsulates safe-init logic for the ADRV9001/ADRV9002 transceiver
 * via libiio. Provides:
 *
 *  - Connection management (libiio context lifecycle)
 *  - Read-only state observation (always safe)
 *  - Safe state transitions (enforces "don't touch" rules)
 *  - Safe calibration sequence
 *  - Profile JSON loading via sysfs profile_config attribute
 *
 * The library refuses to perform dangerous operations identified in
 * the chip_init_constraints documentation:
 *
 *  - Writing to sync_start_enable (driver-internal, userspace writes
 *    break the AXI-ADC bridge)
 *  - Writing initial_calibrations=run while ENSM is in rf_enabled
 *    (wedges the AXI-ADC bridge, requires reboot to recover)
 *
 * Copyright (C) 2026 Open Research Institute and contributors.
 * Licensed under the CERN Open Hardware Licence Version 2 - Strongly
 * Reciprocal (CERN-OHL-S v2). See LICENSE for full text.
 */

#ifndef ORIINIT_H
#define ORIINIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*------------------------------------------------------------------*/
/* Types                                                            */
/*------------------------------------------------------------------*/

/** Opaque handle to a liboriinit context. */
typedef struct oriinit_ctx oriinit_ctx_t;

/** Library status / error codes. Returned by most functions. */
typedef enum {
    ORIINIT_OK = 0,                  /**< Success */
    ORIINIT_ERR_NO_CONTEXT,          /**< libiio context creation failed */
    ORIINIT_ERR_NO_DEVICE,           /**< ADRV9002 PHY device not found */
    ORIINIT_ERR_NO_CHANNEL,          /**< Requested channel not present */
    ORIINIT_ERR_IO,                  /**< libiio read/write failure */
    ORIINIT_ERR_INVALID_ARG,         /**< Bad argument from caller */
    ORIINIT_ERR_INVALID_STATE,       /**< Chip in unexpected state */
    ORIINIT_ERR_DANGEROUS_OP,        /**< Caller tried something on the
                                          don't-touch list */
    ORIINIT_ERR_PROFILE_REJECTED,    /**< Driver rejected profile JSON */
    ORIINIT_ERR_NOT_STREAMING,       /**< Chip didn't end up in rf_enabled */
    ORIINIT_ERR_TIMEOUT,             /**< Operation timed out */
    ORIINIT_ERR_NOT_IMPLEMENTED,     /**< API exists but no backing
                                          implementation yet */
} oriinit_status_t;

/** ADRV9002 ENSM states. Match the chip's terminology. */
typedef enum {
    ORIINIT_ENSM_UNKNOWN = 0,
    ORIINIT_ENSM_CALIBRATED,
    ORIINIT_ENSM_PRIMED,
    ORIINIT_ENSM_RF_ENABLED,
} oriinit_ensm_t;

/** Channel identifiers. Order matches sysfs in_voltage{0,1} / out_voltage{0,1}. */
typedef enum {
    ORIINIT_CH_RX1 = 0,
    ORIINIT_CH_RX2 = 1,
    ORIINIT_CH_TX1 = 2,
    ORIINIT_CH_TX2 = 3,
    ORIINIT_CH_MAX = 4,
} oriinit_channel_t;

/** Snapshot of chip state. Populated by oriinit_read_state(). */
typedef struct {
    oriinit_ensm_t ensm[ORIINIT_CH_MAX];      /**< Per-channel ENSM state */
    bool           enabled[ORIINIT_CH_MAX];   /**< Per-channel enable bit */
    long long      sample_rate_hz;            /**< Effective sample rate */
    long long      lo_frequency_hz[ORIINIT_CH_MAX]; /**< Per-channel LO */
    int            hardware_gain_db[ORIINIT_CH_MAX]; /**< Per-channel gain */
    bool           initial_calibrations_running;     /**< true while cal active */
} oriinit_state_t;

/*------------------------------------------------------------------*/
/* Connection management                                            */
/*------------------------------------------------------------------*/

/**
 * Create a new liboriinit context.
 *
 * @param iio_uri  libiio URI ("local:", "ip:10.73.1.16", etc.).
 *                 Pass NULL or "" for the default local context.
 * @return Pointer to opaque context, or NULL on failure.
 *
 * The returned context must be released with oriinit_destroy().
 */
oriinit_ctx_t *oriinit_create(const char *iio_uri);

/**
 * Release a liboriinit context and its underlying libiio context.
 */
void oriinit_destroy(oriinit_ctx_t *ctx);

/*------------------------------------------------------------------*/
/* State observation (always safe)                                  */
/*------------------------------------------------------------------*/

/**
 * Read the current chip state into a caller-allocated struct.
 *
 * This is a pure observation: no chip-side state is modified.
 *
 * @param ctx  Context.
 * @param out  Destination state struct (must not be NULL).
 * @return ORIINIT_OK on success, error code otherwise.
 */
oriinit_status_t oriinit_read_state(oriinit_ctx_t *ctx,
                                    oriinit_state_t *out);

/**
 * Verify the chip is in a streaming-ready state on the named channel.
 *
 * Checks: channel is enabled AND ENSM is rf_enabled.
 *
 * @param ctx  Context.
 * @param ch   Channel to check.
 * @return ORIINIT_OK if streaming, ORIINIT_ERR_NOT_STREAMING otherwise.
 */
oriinit_status_t oriinit_verify_streaming(oriinit_ctx_t *ctx,
                                          oriinit_channel_t ch);

/*------------------------------------------------------------------*/
/* State manipulation (safe — enforces rules internally)            */
/*------------------------------------------------------------------*/

/**
 * Transition a channel's ENSM to the requested state.
 *
 * Safe transitions: rf_enabled ↔ calibrated ↔ primed. The driver
 * handles intermediate steps; userspace just writes the final target.
 *
 * @param ctx    Context.
 * @param ch     Channel to transition.
 * @param state  Target ENSM state.
 * @return ORIINIT_OK on success.
 */
oriinit_status_t oriinit_set_ensm(oriinit_ctx_t *ctx,
                                  oriinit_channel_t ch,
                                  oriinit_ensm_t state);

/**
 * Run initial calibrations using the safe sequence:
 *   1. Save current ENSM state of RX1 and RX2
 *   2. Drop both to calibrated (precondition for safe cal run)
 *   3. Write initial_calibrations = run
 *   4. Poll until it returns to "off" (or timeout)
 *   5. Restore the saved ENSM states
 *
 * This sequence is the only safe way to invoke calibrations on this
 * driver. Calling initial_calibrations=run from rf_enabled wedges the
 * AXI-ADC bridge and requires a reboot to recover.
 *
 * @param ctx          Context.
 * @param timeout_ms   Max time to wait for cal completion (typical: 3000)
 * @return ORIINIT_OK on success.
 */
oriinit_status_t oriinit_run_calibrations(oriinit_ctx_t *ctx,
                                          unsigned int timeout_ms);

/**
 * Load a profile JSON onto the chip via the profile_config sysfs attribute.
 *
 * Implementation deferred until we have a working profile JSON to test
 * against. Currently returns ORIINIT_ERR_NOT_IMPLEMENTED.
 *
 * @param ctx       Context.
 * @param json_path Path to a TES-exported (or hand-written) profile JSON.
 * @return ORIINIT_OK on success.
 */
oriinit_status_t oriinit_load_profile(oriinit_ctx_t *ctx,
                                      const char *json_path);

/*------------------------------------------------------------------*/
/* Diagnostic / formatting helpers                                  */
/*------------------------------------------------------------------*/

/** Return a static human-readable string for a status code. */
const char *oriinit_status_str(oriinit_status_t status);

/** Return a static human-readable string for an ENSM state. */
const char *oriinit_ensm_str(oriinit_ensm_t state);

/** Return a static human-readable string for a channel identifier. */
const char *oriinit_channel_str(oriinit_channel_t ch);

/**
 * Print a state snapshot to a FILE* (typically stderr or stdout).
 *
 * Format is deliberately stable and machine-parseable (one field per
 * line, "key: value"). Convenient for both eyeballing and for shell
 * scripts that grep the output.
 */
void oriinit_print_state(const oriinit_state_t *state, void *file_ptr);

/*------------------------------------------------------------------*/
/* Version                                                          */
/*------------------------------------------------------------------*/

#define ORIINIT_VERSION_MAJOR 0
#define ORIINIT_VERSION_MINOR 1
#define ORIINIT_VERSION_PATCH 0

/** Return a static version string ("0.1.0"). */
const char *oriinit_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ORIINIT_H */
