/* SPDX-License-Identifier: CERN-OHL-S-2.0 */
/*
 * oriinit-cli — command-line front-end for liboriinit.
 *
 * Subcommands:
 *   status                          read and print current chip state
 *   verify-streaming [rx1|rx2|tx1|tx2]
 *                                   exit 0 if channel is rf_enabled + en
 *   set-ensm <channel> <state>      transition a channel's ENSM
 *   run-calibrations                safe calibration sequence
 *   version                         print library version and exit
 *
 * Global options:
 *   -u <uri>      libiio URI ("local:", "ip:10.73.1.16"); default local
 *
 * Copyright (C) 2026 Open Research Institute and contributors.
 */

#include <oriinit.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int parse_channel(const char *s, oriinit_channel_t *out)
{
    if (!s) return -1;
    if      (strcasecmp(s, "rx1") == 0) { *out = ORIINIT_CH_RX1; return 0; }
    else if (strcasecmp(s, "rx2") == 0) { *out = ORIINIT_CH_RX2; return 0; }
    else if (strcasecmp(s, "tx1") == 0) { *out = ORIINIT_CH_TX1; return 0; }
    else if (strcasecmp(s, "tx2") == 0) { *out = ORIINIT_CH_TX2; return 0; }
    return -1;
}

static int parse_ensm(const char *s, oriinit_ensm_t *out)
{
    if (!s) return -1;
    if      (strcasecmp(s, "rf_enabled") == 0) { *out = ORIINIT_ENSM_RF_ENABLED; return 0; }
    else if (strcasecmp(s, "calibrated") == 0) { *out = ORIINIT_ENSM_CALIBRATED; return 0; }
    else if (strcasecmp(s, "primed")     == 0) { *out = ORIINIT_ENSM_PRIMED;     return 0; }
    return -1;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "oriinit-cli %s — ADRV9002 init/observe utility\n"
        "\n"
        "Usage: %s [-u <uri>] <command> [args]\n"
        "\n"
        "Commands:\n"
        "  status                              read + print chip state\n"
        "  verify-streaming <channel>          exit 0 if channel is rf_enabled\n"
        "  set-ensm <channel> <state>          channel: rx1|rx2|tx1|tx2\n"
        "                                      state:   rf_enabled|calibrated|primed\n"
        "  run-calibrations                    safe initial-calibrations sequence\n"
        "  version                             print version + exit\n"
        "  help                                this message\n"
        "\n"
        "Options:\n"
        "  -u <uri>     libiio URI (default: local:)\n"
        "               e.g. -u ip:10.73.1.16  for remote contexts\n"
        "\n",
        oriinit_version(), prog);
}

static int cmd_status(oriinit_ctx_t *ctx)
{
    oriinit_state_t state;
    oriinit_status_t rc = oriinit_read_state(ctx, &state);
    if (rc != ORIINIT_OK) {
        fprintf(stderr, "oriinit_read_state failed: %s\n",
                oriinit_status_str(rc));
        return 1;
    }
    oriinit_print_state(&state, stdout);
    return 0;
}

static int cmd_verify_streaming(oriinit_ctx_t *ctx, oriinit_channel_t ch)
{
    oriinit_status_t rc = oriinit_verify_streaming(ctx, ch);
    if (rc == ORIINIT_OK) {
        fprintf(stderr, "%s: streaming (rf_enabled + en)\n",
                oriinit_channel_str(ch));
        return 0;
    } else {
        fprintf(stderr, "%s: not streaming (%s)\n",
                oriinit_channel_str(ch), oriinit_status_str(rc));
        return 2;
    }
}

static int cmd_set_ensm(oriinit_ctx_t *ctx,
                        oriinit_channel_t ch, oriinit_ensm_t state)
{
    oriinit_status_t rc = oriinit_set_ensm(ctx, ch, state);
    if (rc != ORIINIT_OK) {
        fprintf(stderr, "set-ensm failed: %s\n", oriinit_status_str(rc));
        return 1;
    }
    fprintf(stderr, "%s -> %s OK\n",
            oriinit_channel_str(ch), oriinit_ensm_str(state));
    return 0;
}

static int cmd_run_calibrations(oriinit_ctx_t *ctx)
{
    fprintf(stderr, "Running calibrations (safe sequence)...\n");
    oriinit_status_t rc = oriinit_run_calibrations(ctx, 5000);
    if (rc != ORIINIT_OK) {
        fprintf(stderr, "calibrations failed: %s\n", oriinit_status_str(rc));
        return 1;
    }
    fprintf(stderr, "calibrations completed OK\n");
    return 0;
}

int main(int argc, char *argv[])
{
    const char *uri = NULL;

    /* Parse global options. */
    int opt;
    while ((opt = getopt(argc, argv, "+u:h")) != -1) {
        switch (opt) {
            case 'u': uri = optarg; break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    if (optind >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    const char *command = argv[optind++];

    /* "version" doesn't need a context. */
    if (strcmp(command, "version") == 0) {
        printf("liboriinit %s\n", oriinit_version());
        return 0;
    }
    if (strcmp(command, "help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    /* All other commands need a context. */
    oriinit_ctx_t *ctx = oriinit_create(uri);
    if (!ctx) {
        fprintf(stderr,
                "oriinit_create failed (uri=%s)\n"
                "  - check libiio context is reachable\n"
                "  - check ADRV9002 PHY device is present\n",
                uri ? uri : "(default local)");
        return 1;
    }

    int exit_code = 1;

    if (strcmp(command, "status") == 0) {
        exit_code = cmd_status(ctx);
    }
    else if (strcmp(command, "verify-streaming") == 0) {
        if (optind >= argc) {
            fprintf(stderr, "verify-streaming: missing channel argument\n");
        } else {
            oriinit_channel_t ch;
            if (parse_channel(argv[optind++], &ch) < 0) {
                fprintf(stderr, "unrecognized channel\n");
            } else {
                exit_code = cmd_verify_streaming(ctx, ch);
            }
        }
    }
    else if (strcmp(command, "set-ensm") == 0) {
        if (optind + 2 > argc) {
            fprintf(stderr, "set-ensm: needs <channel> <state>\n");
        } else {
            oriinit_channel_t ch;
            oriinit_ensm_t state;
            if (parse_channel(argv[optind++], &ch) < 0) {
                fprintf(stderr, "unrecognized channel\n");
            } else if (parse_ensm(argv[optind++], &state) < 0) {
                fprintf(stderr, "unrecognized ENSM state\n");
            } else {
                exit_code = cmd_set_ensm(ctx, ch, state);
            }
        }
    }
    else if (strcmp(command, "run-calibrations") == 0) {
        exit_code = cmd_run_calibrations(ctx);
    }
    else {
        fprintf(stderr, "unknown command: %s\n", command);
        print_usage(argv[0]);
    }

    oriinit_destroy(ctx);
    return exit_code;
}
