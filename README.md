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
├── liboriinit/           ADRV9001/9002 init library (chip configuration
│                         + safe-state enforcement via libiio)
├── services/             systemd services for telemetry + observability
│                           (Hatsuon, dvbs2-mon, kabura-mon, etc.)
└── yocto/                Yocto recipes for packaging into PetaLinux images
```

## Status

Early development. Currently contains:

- ✅ `tools/dma_listen` — first working tool, validates PS↔PL DMA path
- 🚧 `liboriinit` — design phase
- 🚧 `services/*` — design phase
- 🚧 `yocto/*` — design phase

## Building

Each subdirectory has its own Makefile and README with component-specific
build instructions. Build everything from the top:

```sh
make           # build all
make clean     # clean all
```

Or build a single tool:

```sh
cd tools/dma_listen
make
```

See `tools/dma_listen/README.md` for cross-compile instructions (PetaLinux
SDK and manual sysroot approaches both documented).

## Target platform

Designed for PetaLinux 2022.2 rootfs on ZCU102, cross-compiled from a
Linux host. Components vendor their own third-party headers (e.g.,
`tools/dma_listen` vendors `iio.h` v0.25) so builds are immune to the
host's libiio version.

The recommended cross-compile path is via the PetaLinux SDK environment
script; see `tools/dma_listen/README.md` for details. A manual sysroot
approach (Ubuntu's `gcc-aarch64-linux-gnu` + libiio runtime copied from
target) also works and is documented for situations where the SDK isn't
handy.

Runtime dependencies vary by component:
- `dma_listen`: `libiio` v0.x (NOT v1 — incompatible API)
- `liboriinit` (future): `libiio`
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
