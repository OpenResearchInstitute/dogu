# liboriinit

ADRV9001/ADRV9002 initialization library for ORI SDR builds.

Encapsulates safe-init logic for the Analog Devices ADRV9001/ADRV9002
transceiver via libiio. Provides connection management, read-only state
observation, safe ENSM transitions, a safe initial-calibrations sequence
(with 1T1R configuration support), and a placeholder for profile loading
once the implementation pass lands.

## Why this exists

The ADRV9001/9002 driver has sharp edges that bite the unwary. Three
in particular:

1. **Writing to `sync_start_enable` from userspace** confuses the driver's
   internal bookkeeping. This attribute is exposed in sysfs but is
   *driver-internal* — userspace writes have no useful effect and can
   contribute to bridge wedges. liboriinit never writes to it.

2. **Writing `initial_calibrations=run` while ENSM is in `rf_enabled`**
   wedges the FPGA-side AXI-ADC bridge unrecoverably. Recovery requires
   a board reboot. liboriinit refuses this operation by enforcing a safe
   sequence: snapshot state → drop active RX channels to `calibrated` →
   run → poll for completion → restore.

3. **Disabled channels in 1T1R configurations** (e.g., RX2 / TX2 after
   loading a 1T1R FDD profile) report `ensm: unknown` and `enabled: no`.
   Writing `ensm_mode` to a disabled channel returns `-EIO`. The
   calibration safe-sequence has to skip these channels, not blindly
   iterate over all four. liboriinit does this gating; see
   `src/oriinit.c` `oriinit_run_calibrations()`.

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
The implementation prerequisites are now resolved (TES 0.23.1 secured,
working JSON profiles validated end-to-end via the shell shortcut
`cat profile.json > /sys/bus/iio/devices/iio:device1/profile_config`).
The TODO comment in `src/oriinit.c` documents the work plan; it's
scheduled for the next liboriinit session.

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

`liboriinit` builds a plain `liboriinit.so` (no version suffix, no
SONAME machinery) plus the `oriinit-cli` binary that links against it.
The CLI's rpath is `$ORIGIN`, so it finds the library in the same
directory at runtime — no system-wide install required.

Uses a vendored `iio.h` v0.25 so the build is immune to whichever
libiio version the host has installed (or whether libiio is installed
at all on the host).

### Quick reference — use the top-level Makefile

From the dōgu repo root:

```sh
make                                 # native build (host arch, for syntax checking)
make cross                           # aarch64 cross-compile (default sysroot)
make cross SYSROOT=/custom/path      # aarch64 with custom sysroot
make deploy                          # scp aarch64 binaries to the target board
make clean                           # clean both subdirectories
```

### Direct invocation from this subdirectory

```sh
# Native
make

# Manual cross-compile with SYSROOT
make CC=aarch64-linux-gnu-gcc SYSROOT=$HOME/aarch64-sysroot

# Or via PetaLinux SDK environment (sets CC + sysroot automatically)
source /opt/petalinux/2022.2/environment-setup-cortexa72-cortexa53-xilinx-linux
make
```

### Build artifacts

```
liboriinit.so       # the library (single file, no symlinks)
oriinit-cli         # the CLI tool
src/oriinit.o       # intermediate object
```

Both `liboriinit.so` and `oriinit-cli` end up next to each other in
`liboriinit/` after a successful build. Drop both into the same
directory on the target and the CLI will find the library via its
`$ORIGIN` rpath.

## Deploying to the target

The top-level Makefile has a `deploy` target that handles this
end-to-end. After `make cross`:

```sh
make deploy
# defaults: DEPLOY_HOST=root@10.73.1.16  DEPLOY_PATH=/home/root
# override either:
make deploy DEPLOY_HOST=root@haifuraiya.local DEPLOY_PATH=/opt/ori
```

`make deploy` removes any stale `liboriinit.so*` files from previous
deployments (including the .so.0 / .so.0.1.0 symlink chain produced
by older versions of the Makefile), then scp's the fresh binaries.
One command, repeatable, idempotent.

Manual equivalent if you need it:

```sh
ssh root@<board> 'rm -f /home/root/liboriinit.so*'
scp tools/dma_listen/dma_listen root@<board>:/home/root/
scp liboriinit/oriinit-cli       root@<board>:/home/root/
scp liboriinit/liboriinit.so     root@<board>:/home/root/
```

Or, when a Yocto recipe lands in `dogu/yocto/`, the library + CLI ride
into the PetaLinux image alongside the rest of the rootfs.

## Initial sanity test on the board

Once deployed:

```sh
# Confirm CLI runs
/home/root/oriinit-cli version
# → 0.1.0

# Read chip state
/home/root/oriinit-cli status
```

Expected output after a 1T1R FDD CMOS profile load:

```
sample_rate_hz: 1920000
initial_calibrations_running: no
RX1_ensm: rf_enabled
RX1_enabled: yes
RX1_gain_db: 34
RX1_lo_hz: 2400000000
RX2_ensm: unknown            # ← 1T1R: RX2 disabled
RX2_enabled: no
RX2_gain_db: 0
RX2_lo_hz: 0
TX1_ensm: rf_enabled
TX1_enabled: yes
TX1_gain_db: -10
TX1_lo_hz: 2450000000
TX2_ensm: unknown            # ← 1T1R: TX2 disabled
TX2_enabled: no
TX2_gain_db: 0
TX2_lo_hz: 0
```

For 2T2R profiles, RX2 and TX2 would show `rf_enabled / enabled: yes`
instead. `run-calibrations` handles both configurations transparently:
it iterates over RX1 + RX2, skipping any channel reporting `ensm: unknown`.

## What's implemented vs. deferred

- ✅ Connection management (create / destroy)
- ✅ State observation (read_state with sample rate, ENSM, enables, gains, LO)
- ✅ Streaming verification (verify_streaming)
- ✅ Safe ENSM transitions (set_ensm)
- ✅ Safe initial-calibrations sequence (run_calibrations), including 1T1R
- ✅ String helpers for status/ENSM/channel
- 🚧 Profile JSON loading (stubbed; prerequisites resolved, implementation
   scheduled for the next liboriinit session — see `oriinit_load_profile()`
   TODO comment in `src/oriinit.c`)
- 🚧 More attribute coverage (RSSI, calibration table inspection, etc.)
- 🚧 systemd-friendly oneshot wrapper (will land in `dogu/services/oriinit/`)

## Verified bench history

- **2026-05-26** — v0.1 shipped with state observation + 2T2R safe-sequence
  calibrations. Verified on ZCU102+ADRV9002 with default 2T2R FDD CMOS
  profile.
- **2026-05-27** — 1T1R support added (`run_calibrations` skips disabled
  channels). Verified end-to-end on ZCU102: TES-generated 1T1R FDD CMOS
  profile loaded via shell shortcut → `oriinit-cli run-calibrations`
  completes cleanly → real ADC samples flowing through the channelizer
  (`dma_listen` shows non-zero samples, RMS ≈ 1 LSB consistent with
  terminated-input thermal noise floor).

## Layout

```
liboriinit/
├── README.md                       (this file)
├── Makefile                        builds liboriinit.so + CLI + install target
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

CERN Open Hardware Licence Version 2 — Strongly Reciprocal (CERN-OHL-S v2).

The vendored `third_party/libiio-v0.25/iio.h` retains its original
LGPL-2.1-or-later licensing.
