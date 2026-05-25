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

## Build

Native build on the ZCU102 (after scp'ing the source):

```sh
make
```

Cross-compile from a host with the aarch64 toolchain:

```sh
make CC=aarch64-linux-gnu-gcc
```

## Usage

```
./dma_listen [options] [uri]

Options:
  -n <samples>   buffer size in samples (default: 4096)
  -c <count>     stop after N refills (default: run until Ctrl-C)
  -d <dev>       device name (default: axi-adrv9002-rx-lpc)
  -h             help

uri              libiio context URI (default: local:)
                 e.g. 'ip:10.73.1.16' to connect remotely
```

## Example output

Default-profile state (the "no real ADC data" case):

```
libiio: using default (local) context
libiio: context has 8 devices
libiio: created buffer for 4096 samples (~16384 bytes per refill)
libiio: sample rate 1.920 Msps → each refill covers 2.13 ms

--- starting refill loop (Ctrl-C to stop) ---
[0.012s] 4096 samples (16384 B, 1.37 MB/s) | I[-1..0] mean=-0.5 |
  Q[-1..0] mean=-0.5 | RMS=1.0 | bin=99.2% sat=0.0% distinct_high_I=2 |
  DEFAULT-PROFILE BINARY (no real ADC)
[0.024s] ...
```

When a custom profile is loaded and produces real ADC data, the
classifier flips to `MULTI-BIT (real ADC samples?)` — same code,
different chip state.

## Dependencies

- `libiio` (runtime + headers)
- `libm`
- GCC or clang

## When to use this tool

- **First-time board bring-up**: confirms libiio + PL plumbing work
- **After loading a new ADRV9002 profile**: confirms the chip is
  emitting useful samples at the expected rate
- **Debugging AXI-ADC bridge issues**: per-refill stats help isolate
  whether the chip stopped emitting vs. the bridge wedged
- **Capturing I/Q for offline demod**: redirect stdout (NOT stderr —
  stderr is the stats stream) to capture binary samples
