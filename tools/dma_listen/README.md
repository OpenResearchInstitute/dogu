# dma_listen

A PS↔PL DMA path validator for ZCU102 / ADRV9001 / ADRV9002 builds.

Opens an libiio context, creates a buffer on the AXI-ADC RX device,
loops on `iio_buffer_refill()`, and prints summary statistics per
refill: byte count, distinct sample values, magnitude stats, and a
classifier that distinguishes the default-profile binary 0x00/0xFF
pattern from real multi-bit ADC data.

## Purpose

In the Haifuraiya transponder architecture, the demod and all
high-rate signal processing live entirely in the PL — the ARM never
sees per-sample I/Q data in production. So this tool is **not** the
skeleton of a daemon. It's a diagnostic, used for:

- Confirming the AXI-ADC bridge is functional (PS can DMA from PL)
- Detecting the default-profile 0x00/0xFF binary pattern signature
- Validating a custom profile produces multi-bit data
- Capturing raw I/Q files for offline analysis with `opv-cxx-demod`
  or other tools

Run it manually during bring-up, profile development, or when
debugging "is the chip actually streaming useful samples?" questions.

## Building

The Makefile uses a vendored copy of `iio.h` at v0.25 (matching the
runtime in PetaLinux 2022.2). This means the build is immune to whether
the system has `libiio-dev` installed or has libiio v1 installed (the
new API is incompatible with v0.x).

### Quick reference — use the top-level Makefile

From the dōgu repo root:

```sh
make                              # native build (host arch)
make cross                        # aarch64 cross-compile (default sysroot)
make cross SYSROOT=/custom/path   # aarch64 with custom sysroot
make deploy                       # ship aarch64 binaries to the target
```

### Direct invocation from this subdirectory

#### Native build on the target

Easiest when the target rootfs has `gcc` (custom PetaLinux image with
development packages) or you've added a compiler post-install:

```sh
make
```

The default PetaLinux 2022.2 minimal rootfs does NOT include `gcc`
or `make`. You'll need to cross-compile.

#### Cross-compile via PetaLinux SDK (cleanest if you have the SDK)

If you have the PetaLinux SDK installed (you do, if you built the
PetaLinux image at all), source its environment-setup script and
the build "just works":

```sh
source /opt/petalinux/2022.2/environment-setup-cortexa72-cortexa53-xilinx-linux
# (path may differ depending on where PetaLinux is installed)
make
```

This gives a clean build with a complete sysroot — libiio's transitive
dependencies (libusb, libavahi, libxml2, libserialport) all available
at link time, no `--allow-shlib-undefined` workaround needed.

#### Cross-compile with the Vitis-bundled toolchain (the default path)

This is what the top-level `make cross` uses. The Xilinx/Vitis install
ships an aarch64 GNU toolchain; combined with a minimal sysroot for
libiio, it produces deployable binaries.

```sh
# One-time setup: pull libiio runtime from the target
mkdir -p ~/aarch64-sysroot/lib
scp root@<board>:/usr/lib/libiio.so.0 ~/aarch64-sysroot/lib/
ln -sf libiio.so.0 ~/aarch64-sysroot/lib/libiio.so

# Then build (the Vitis toolchain + SYSROOT)
make CROSS_COMPILE=/opt/Xilinx/Vitis/2022.2/gnu/aarch64/lin/aarch64-linux/bin/aarch64-linux-gnu- \
     SYSROOT=$HOME/aarch64-sysroot
```

The Makefile expands `SYSROOT` into the appropriate `-L$(SYSROOT)/lib`
and `-Wl,--allow-shlib-undefined` flags. The latter tells the linker
"I know libiio has dependencies; trust me, they'll be there at runtime
on the target." Which is true — `libusb`, `libavahi-*`, `libxml2`, and
`libserialport` are standard parts of the target rootfs even when
libiio-dev isn't installed.

> **Why the Vitis toolchain and not Ubuntu's `gcc-aarch64-linux-gnu`?**
> The Ubuntu apt package conflicts with `gcc-multilib` (which PetaLinux
> requires) — installing one evicts the other, so you end up in a loop
> where building PetaLinux breaks your cross-compiler and vice versa.
> The Vitis toolchain lives outside apt, dodging the conflict, and is
> ABI-matched (GCC 11.2/glibc) to the PetaLinux 2022.2 target. The
> top-level Makefile defaults to it for exactly this reason.

### Deploying

Via the top-level Makefile's `deploy` target (ships dma_listen along
with the other dōgu binaries):

```sh
cd ../..
make deploy
# defaults: DEPLOY_HOST=root@10.73.1.16  DEPLOY_PATH=/home/root
```

Or manually:

```sh
scp dma_listen root@<board>:/path/to/destination/
```

## Usage

```
./dma_listen [options] [uri]

Options:
  -n <samples>   buffer size in samples (default: 4096)
                 NOTE: AXI-Stream tlast may make the actual per-refill
                 sample count smaller than this — see "Per-refill size"
                 below.
  -c <count>     stop after N refills (default: run until Ctrl-C)
  -d <dev>       device name (default: axi-adrv9002-rx-lpc)
  -h             help

uri              libiio context URI (default: local:)
                 e.g. 'ip:10.73.1.16' to connect remotely
```

## Per-refill size

In the Haifuraiya build, each `iio_buffer_refill()` returns ~64 samples
even when a larger buffer is requested. That's because the upstream
channelizer in the PL asserts AXI-Stream `tlast` after each complete
pass through its 64 channel outputs, so the DMA returns at tlast.

This is *useful information about the data path*, not a bug:

- Each refill = one snapshot across all 64 channelizer channels
- Sample ordering within a refill is by AXI-Stream `tdest` (channel 0,
  channel 1, ..., channel 63)
- For non-channelizer-fed AXI-ADC paths, larger refills are expected

## Example output

### No real samples — default profile or pre-calibration state

```
libiio: using default (local) context
libiio: context has 29 devices
libiio: created buffer for 4096 samples (~16384 bytes per refill)
libiio: sample rate 1.920 Msps -> each refill covers 2.13 ms

--- starting refill loop (Ctrl-C to stop) ---
[0.012s] 64 samples (256 B, 4.57 MB/s) | I[0..0] mean=0.0 |
  Q[0..0] mean=0.0 | RMS=0.0 | bin=100.0% sat=0.0% distinct_high_I=1 |
  DEFAULT-PROFILE BINARY (no real ADC)
```

Diagnostic: every sample is literally 0/0, `distinct_high_I=1` (only one
value seen). The chip isn't producing ADC output — either the profile
isn't loaded, or initial calibrations haven't run.

### Calibrated chip with terminated input (real but weak ADC samples)

```
[1.005s] 64 samples (256 B, 20.83 MB/s) | I[-1..0] mean=-0.5 |
  Q[-1..0] mean=-0.5 | RMS=1.0 | bin=100.0% sat=0.0% distinct_high_I=2 |
  DEFAULT-PROFILE BINARY (no real ADC)
```

Note the differences from the above:
- I/Q values toggle between -1 and 0 (not stuck at 0)
- RMS ≈ 1 LSB
- `distinct_high_I=2` (two distinct values in I, not one)

The classifier still flags it as "DEFAULT-PROFILE BINARY" because the
heuristic looks for amplitude beyond the LSB level. But the chip IS
producing real ADC samples — they're just thermal-noise-floor from a
50Ω terminated input, at the very bottom of the dynamic range.

To prove the receiver works for *signals*, inject a CW from another
SDR at the configured RX LO frequency. Expected behavior: RMS rises
significantly, `distinct_high_I` jumps to many distinct values, and
the classifier flips to `MULTI-BIT (real ADC samples?)`.

## Dependencies

**Runtime** (on the target):
- `libiio.so.0` (version 0.25 or compatible v0.x)
- `libusb-1.0`, `libavahi-client`, `libavahi-common`, `libxml2`,
  `libserialport` — libiio's own dependencies

**Build time**:
- `gcc` or `clang` (native or aarch64-cross)
- `libm`
- No need for `libiio-dev` — we vendor `iio.h` v0.25 in `third_party/`

## When to use this tool

- **First-time board bring-up**: confirms libiio + PL plumbing work
- **After loading a new ADRV9002 profile**: confirms the chip is
  emitting useful samples at the expected rate
- **After running `oriinit-cli run-calibrations`**: confirms cal
  brought the RX path fully alive (pre-cal → post-cal transition
  shifts samples from "all zero" to "LSB-level thermal noise")
- **Debugging AXI-ADC bridge issues**: per-refill stats help isolate
  whether the chip stopped emitting vs. the bridge wedged
- **Capturing I/Q for offline demod**: redirect stdout (NOT stderr —
  stderr is the stats stream) to capture binary samples
