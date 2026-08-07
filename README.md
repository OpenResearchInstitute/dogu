# 道具 — dōgu

**ORI ARM-side tooling for ADRV9001/ADRV9002 SDR builds.**

Open Research Institute · CERN-OHL-S v2

---

## What is this?

A collection of ARM-userspace software for ORI radios built around the
Analog Devices ADRV9001/9002 transceiver — chip-init libraries, diagnostic
tools, telemetry publishers, and dashboards.

Primary platform: ZCU102 + ADRV9002 (Haifuraiya mode-dynamic transponder).
Secondary platforms: any Linux-on-Zynq or Linux-on-ZynqMP target with an
ADRV9001/9002, including custom carrier boards.

The name *dōgu* (道具) is Japanese for "tool" or "implement" — a deliberately
broad container for the growing collection of pieces that support ORI's
ADRV-based radios.

## What's in here

```
dogu/
├── tools/                Standalone diagnostic / development utilities
│   └── dma_listen/         Validate libiio + AXI-ADC DMA path
|   └── frame_send/         Intermediate test - send frames to Interlocutor
|   └── seam_grade/         Grade the quality of the demodulated frames
├── liboriinit/           ADRV9001/9002 init library (chip configuration
│                         + safe-state enforcement via libiio)
├── frame_decoder/        A53 OPV frame decoder (opv-cxx-demod submodule):
│                         soft bits from the PL -> decoded frames -> Interlocutor
├── services/             systemd services for telemetry + observability
│                           (Hatsuon, dvbs2-mon, kabura-mon, etc.)
└── yocto/                Yocto recipes for packaging into PetaLinux images
```

## Status

## Status
- ✅ `tools/dma_listen` — PS↔PL DMA path validator and soft-bit capture
  tool. 16-bit DMA word contract aware (self-checking high-byte verify,
  see WP_SOFTBIT_FORMAT_CONTRACT.md); writes de-stuffed captures in
  opv-decode -3 layout. Verified working on ZCU102+ADRV9002.
- ✅ `tools/frame_send` — intermediate test tool: send frames to
  Interlocutor.
- 🧪 `tools/seam_grade` — receiver quality grader for the periodic
  frame-quality collapse (WP_SYMBOL_CLOCK_SLIP). Merges the dma_listen
  arrival timeline with opv-decode -3 FEC metrics to classify every
  40 ms slot OK / DEGRADED / ABSENT, measure the slip period in
  wall-clock time, and render a PASS/FAIL verdict (`make test-seam`).
  Rig verified end-to-end on dead air (stall watchdog, exit-code
  taxonomy 0/1/2/3); first graded live capture pending.
- ✅ `frame_decoder` — A53 `opv-decode` from the opv-cxx-demod submodule;
  cross-builds for aarch64, ships via `make deploy`, self-tests via
  `make test-frame-decoder`.
- ✅ `liboriinit` v0.1 — chip-init library with state observation,
  safe-sequence calibrations (1T1R + 2T2R), and CLI. Profile-load
  function stubbed pending one more implementation pass.
- ✅ `yocto/` — recipes packaging dogu components into PetaLinux images.
  Bouro v0 (MQTT telemetry publisher + dashboard) ships this way and is
  live on-target: bouro.service publishes channelizer telemetry via
  devmem → mosquitto; the dashboard is served by mosquitto's websockets
  listener at http://<target>:9001/bouro.html (no separate webserver —
  the 9001 listener does both).
- 🚧 `services/` — placeholder; not currently load-bearing. Service
  definitions live with their yocto recipes (see Bouro). Retained for
  future components that need staging before recipe-ization, or retire
  it if that never materializes.

## Cloning

There is a submodule involved. Dogu pulls in the C++ modem repository 
and uses that code to build `frame_decoder`. 

```
git clone https://github.com/OpenResearchInstitute/dogu
cd ~/dogu
git submodule status        # a leading '-' means uninitialized
git submodule update --init --recursive
```

## Building

dōgu uses a unified build system. From the repo root:

```sh
make                              # native build for host (syntax-check usage)
make cross                        # aarch64 cross-compile for target deployment
make deploy                       # ship aarch64 binaries to the ZCU102
make clean                        # clean all subdirectories
make help                         # print available targets + variables
```

The full bring-it-fresh-to-the-board sequence is three commands:

```sh
make clean && make cross && make deploy
```

Check out `http://10.73.1.16:9001/bouro.html` to see MQTT driven web presentation of radio health.

### Cross-compile defaults

| Variable | Default | Notes |
|---|---|---|
| `XILINX_GNU_DIR` | `/opt/Xilinx/Vitis/2022.2/gnu/aarch64/lin/aarch64-linux/bin` | Directory holding the Vitis-bundled `aarch64-linux-gnu-gcc` |
| `CROSS_COMPILE` | `$(XILINX_GNU_DIR)/aarch64-linux-gnu-` | Toolchain prefix (used as `$(CROSS_COMPILE)gcc`) |
| `SYSROOT` | `$HOME/aarch64-sysroot` | Path to aarch64 sysroot with libiio runtime |
| `DEPLOY_HOST` | `root@10.73.1.16` | Board ssh target |
| `DEPLOY_PATH` | `/home/root` | Directory on the target |

Override any of them on the command line:

```sh
make cross XILINX_GNU_DIR=/opt/Xilinx/Vitis/2023.2/gnu/aarch64/lin/aarch64-linux/bin
make deploy DEPLOY_HOST=root@haifuraiya.local DEPLOY_PATH=/opt/ori
```

### Building a single component

```sh
cd tools/dma_listen && make                                          # native
cd tools/dma_listen && make CROSS_COMPILE=$XILINX_GNU_DIR/aarch64-linux-gnu- SYSROOT=...  # cross
```

See each subdirectory's `README.md` for component-specific build notes.

### Cross-compile toolchain (important — read this)

dōgu cross-compiles with the **Xilinx/Vitis-bundled aarch64 GNU toolchain**,
which the `make cross` default (`XILINX_GNU_DIR`) points at. **Do not use
Ubuntu's `gcc-aarch64-linux-gnu` apt package for this.** That package
*conflicts* with `gcc-multilib`, which PetaLinux requires — installing one
makes apt evict the other. The result is a maddening loop where setting up
to build PetaLinux silently uninstalls your cross-compiler, and setting up
to cross-compile dōgu silently breaks PetaLinux. The Vitis toolchain lives
outside apt, so it sidesteps the conflict entirely, and it's ABI-matched
(GCC 11.2 / glibc) to the PetaLinux 2022.2 target.

If your Vitis install path or version differs, override `XILINX_GNU_DIR`:

```sh
make cross XILINX_GNU_DIR=/path/to/Vitis/<version>/gnu/aarch64/lin/aarch64-linux/bin
```

### Cross-compile sysroot setup (one-time)

The toolchain provides the compiler; the `SYSROOT` provides libiio for the
linker. A minimal sysroot just needs libiio's shared library copied from
the target:

```sh
mkdir -p ~/aarch64-sysroot/lib
scp root@<board>:/usr/lib/libiio.so.0 ~/aarch64-sysroot/lib/
ln -sf libiio.so.0 ~/aarch64-sysroot/lib/libiio.so
```

That's it. dōgu's vendored headers (`third_party/libiio-v0.25/iio.h`)
plus this one `.so` are enough for the linker to produce working
aarch64 binaries against the target's libiio v0.25 runtime. The
Makefile passes `--allow-shlib-undefined` automatically so the linker
doesn't complain about libiio's transitive dependencies (libusb,
libavahi, libxml2, libserialport) — those resolve at runtime on the
target.

The PetaLinux SDK may be needed with a fresh install. It is also a 
valid toolchain+sysroot source — build it (`petalinux-build --sdk`), 
install it, source its environment-setup script, and `make` picks 
up `CC` and the sysroot automatically. This is the most ABI-rigorous 
option since the SDK is generated from the same project that builds 
the target rootfs.

```
cd ~/purple/Mode-Dynamic-Transponder/haifuraiya/petalinux/haifuraiya
petalinux-build --sdk
./images/linux/sdk.sh -d ~/petalinux-sdk
```

## Switching between native and cross builds

`make` doesn't track architecture in dependency timestamps, so if you've
just done a native build and then run `make cross`, Make will say
"Nothing to be done" because the existing artifacts look up-to-date.
Run `make clean` between architecture switches:

```sh
make clean && make cross
make clean && make
```

## Target platform

Designed for PetaLinux 2022.2 rootfs on ZCU102, cross-compiled from a
Linux host. Components vendor their own third-party headers (e.g.,
`tools/dma_listen` and `liboriinit` both vendor `iio.h` v0.25) so
builds are immune to the host's libiio version.

Runtime dependencies vary by component:

- `dma_listen`: `libiio` v0.x (NOT v1 — incompatible API)
- `liboriinit`: `libiio` v0.x (same constraint)
- `services/*` (future): `mosquitto`, `paho-mqtt`

## License

CERN Open Hardware Licence Version 2 — Strongly Reciprocal (CERN-OHL-S
v2). See `LICENSE` for full text.

The CERN-OHL-S v2 is the license used across ORI's hardware and HDL
work; dōgu uses the same license for symmetry, even though it consists
primarily of software. This means: you can use, modify, and redistribute
this code, but if you make a hardware product based on it (or modify
the code and distribute the result), your modifications must also be
released under CERN-OHL-S v2.

## Contributing

ORI welcomes contributions. See `CONTRIBUTING.md`.

## Related ORI repositories

- **Mode-Dynamic-Transponder** — Haifuraiya HDL design (channelizer,
  demod farm, DVB-S2 chain, etc.)
- **pluto_msk** — MSK modem HDL (the demod farm inherits its inner
  engine from here)
- **opv-cxx-demod** — C++ reference implementation of the OPV demod
  (verification oracle, not a port target)
- **Dialogus** — receiver software for Interlocutor / LibreSDR; the
  conceptual precedent for dōgu
