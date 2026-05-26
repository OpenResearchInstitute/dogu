# liboriinit

ADRV9001/ADRV9002 initialization library for ORI SDR builds.

Encapsulates safe-init logic for the Analog Devices ADRV9001/ADRV9002
transceiver via libiio. Provides connection management, read-only state
observation, safe ENSM transitions, and a safe initial-calibrations
sequence. Profile loading is stubbed pending a working TES-generated
profile JSON for our driver API version.

## Why this exists

The ADRV9001/9002 driver has sharp edges that bite the unwary. Two in
particular:

1. **Writing to `sync_start_enable` from userspace** confuses the driver's
   internal bookkeeping. This attribute is exposed in sysfs but is
   *driver-internal* — userspace writes have no useful effect and can
   contribute to bridge wedges. liboriinit never writes to it.

2. **Writing `initial_calibrations=run` while ENSM is in `rf_enabled`**
   wedges the FPGA-side AXI-ADC bridge unrecoverably. Recovery requires
   a board reboot. liboriinit refuses this operation by enforcing a safe
   sequence: snapshot state → drop to `calibrated` → run → poll for
   completion → restore.

Encoding these rules in a library means each new daemon, tool, or
script doesn't need to re-discover them by breaking the board.

## API

See `include/oriinit.h` for the full public API. Highlights:

```c
oriinit_ctx_t *oriinit_create(const char *iio_uri);
void           oriinit_destroy(oriinit_ctx_t *ctx);

oriinit_status_t oriinit_read_state(oriinit_ctx_t *, oriinit_state_t *);
oriinit_status_t oriinit_verify_streaming(oriinit_ctx_t *, oriinit_channel_t);

oriinit_status_t oriinit_set_ensm(oriinit_ctx_t *, oriinit_channel_t,
                                   oriinit_ensm_t);
oriinit_status_t oriinit_run_calibrations(oriinit_ctx_t *, unsigned timeout_ms);
oriinit_status_t oriinit_load_profile(oriinit_ctx_t *, const char *json_path);
```

`oriinit_load_profile()` currently returns `ORIINIT_ERR_NOT_IMPLEMENTED`.
It will be filled in once we have a TES-generated profile JSON matching
our PetaLinux 2022.2 driver's API version (68.5.0, which requires
TES 0.23.1 — not currently available from ADI).

## Companion CLI: `oriinit-cli`

Built alongside the library. Exercises the API from the command line:

```sh
oriinit-cli status                  # read + print current chip state
oriinit-cli verify-streaming rx1    # exit 0 if RX1 is rf_enabled + en
oriinit-cli set-ensm rx1 calibrated # transition RX1's ENSM
oriinit-cli run-calibrations        # safe-sequence calibration
oriinit-cli version                 # print library version
```

Useful as a quick "is the chip alive and in the expected state?" check
during bring-up.

## Building

Same model as `tools/dma_listen`: cross-compile via PetaLinux SDK
(recommended), manual aarch64 sysroot, or native build if the rootfs
has `gcc`. Uses a vendored `iio.h` v0.25 to be immune to whatever libiio
version the build host has installed.

```sh
# Cross-compile via PetaLinux SDK
source /opt/petalinux/2022.2/environment-setup-cortexa72-cortexa53-xilinx-linux
make

# Or manual cross-compile
make CC=aarch64-linux-gnu-gcc \
     LDFLAGS="-L$HOME/aarch64-sysroot/lib -Wl,--allow-shlib-undefined"
```

Build produces:

- `liboriinit.so.0.1.0` (versioned shared library)
- `liboriinit.so.0` and `liboriinit.so` (symlinks)
- `oriinit-cli` (executable, links against the .so via `$ORIGIN` rpath)

The CLI's rpath is set to `$ORIGIN` so it finds the .so in the same
directory during development. The install target (`make install`)
places the library in `$(PREFIX)/lib` and the CLI in `$(PREFIX)/bin`
where the system loader's normal search resolves things.

## Deploying to the target

```sh
# Ship the library and CLI together
scp liboriinit.so.0.1.0 root@<board>:/usr/lib/
ssh root@<board> "cd /usr/lib && ln -sf liboriinit.so.0.1.0 liboriinit.so.0 && ln -sf liboriinit.so.0 liboriinit.so && ldconfig"
scp oriinit-cli root@<board>:/usr/local/bin/
```

Or, when a Yocto recipe lands in `dogu/yocto/`, the library + CLI ride
into the PetaLinux image alongside the rest of the rootfs.

## Initial sanity test on the board

Once deployed:

```sh
# Confirm CLI runs
oriinit-cli version

# Read chip state — should match what we know from prior sysfs probing
oriinit-cli status

# Expected output (with default-profile boot state):
#   sample_rate_hz: 1920000
#   initial_calibrations_running: no
#   RX1_ensm: rf_enabled
#   RX1_enabled: yes
#   RX1_gain_db: <whatever the default is>
#   RX1_lo_hz: <whatever the default LO is>
#   RX2_ensm: rf_enabled
#   ...
```

## What's implemented vs. deferred

✅ Connection management (create / destroy)
✅ State observation (read_state with sample rate, ENSM, enables, gains, LO)
✅ Streaming verification (verify_streaming)
✅ Safe ENSM transitions (set_ensm)
✅ Safe initial-calibrations sequence (run_calibrations)
✅ String helpers for status/ENSM/channel
🚧 Profile JSON loading (stubbed; awaiting TES 0.23.1 or equivalent)
🚧 More attribute coverage (RSSI, calibration table inspection, etc.)
🚧 systemd-friendly oneshot wrapper (will land in `dogu/services/oriinit/`)

## Layout

```
liboriinit/
├── README.md                       (this file)
├── Makefile                        builds .so + CLI + install target
├── include/
│   └── oriinit.h                   public API header
├── src/
│   └── oriinit.c                   implementation
├── cli/
│   └── oriinit-cli.c               CLI tool exercising the library
└── third_party/
    └── libiio-v0.25/
        ├── iio.h                   vendored libiio v0.25 header
        └── README.md               provenance + license
```

## License

CERN Open Hardware Licence Version 2 - Strongly Reciprocal (CERN-OHL-S v2).

The vendored `third_party/libiio-v0.25/iio.h` retains its original
LGPL-2.1-or-later licensing.
