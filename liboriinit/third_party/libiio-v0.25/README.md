# third_party/libiio-v0.25

Vendored copy of `iio.h` from libiio v0.25, used by `dma_listen` to build
against the libiio API that ships in PetaLinux 2022.2 (and similar BSPs
of that era).

## Why vendored

1. **Pinning**: libiio's `main` branch became v1.x with a *deliberately
   incompatible* new API in August 2023. Any contributor with a recent
   distribution's `libiio-dev` installed will have v1 headers — building
   against those produces broken binaries that fail at runtime against
   v0.x runtime libraries. Vendoring v0.25 makes the dōgu build immune
   to the system's libiio version.

2. **Self-contained builds**: cross-compile environments (e.g., a fresh
   Ubuntu install with the `gcc-aarch64-linux-gnu` package) don't have
   any libiio headers. Vendoring eliminates one dependency from setup.

3. **Reproducibility**: the API surface our code uses is fixed at
   v0.25 forever, regardless of what happens upstream.

## File provenance

| File | Source | Version | Retrieved |
|---|---|---|---|
| `iio.h` | https://github.com/analogdevicesinc/libiio | tag `v0.25` (commit `b6028fd`) | 2026-05-25 |

Original URL:
`https://raw.githubusercontent.com/analogdevicesinc/libiio/v0.25/iio.h`

## License

`iio.h` is **LGPL-2.1-or-later**, Copyright (C) 2014 Analog Devices, Inc.
The license header is preserved verbatim at the top of the file.

LGPL-licensed headers are designed to be included in non-LGPL projects;
the LGPL governs only this specific file, not the consuming code (dōgu's
CERN-OHL-S v2 license applies to dōgu's own sources). Compiled programs
that link against `libiio.so` at runtime are governed by LGPL's dynamic-
linking allowance.

If you modify this file, the modifications must remain under LGPL-2.1-or-later
and should be considered for upstream contribution to libiio rather than
maintained here as a fork.

## Updating

If libiio v0.x receives important bug fixes that affect the API surface
we use, refresh this vendored copy:

```sh
wget -O iio.h \
  https://raw.githubusercontent.com/analogdevicesinc/libiio/<TAG>/iio.h
```

Pick a tag, not the `libiio-v0` branch tip, so the vendored version is
reproducible. Update the table above when you do.

**Do NOT** fetch from `main` — that's v1, incompatible API.
