# frame_decoder

The A53-side **OPV frame decoder** for the fabric-demod split — a thin dogu
wrapper around the [`opv-cxx-demod`](https://github.com/OpenResearchInstitute/opv-cxx-demod)
submodule that builds and ships one binary: **`opv-decode`**.

## Where it sits in the chain

```
PL fabric (hard real time):
  channelizer  ->  MSK demod  ->  frame sync detector (soft bits)
  [Mode-Dynamic-Transponder] [msk_demodulator]   [pluto_msk]
        |
        |  AXI-Stream — a FRAME. First stage with NO hard real-time requirement.
        v
A53 (this component):
  opv-decode  ->  sync-resolve + deinterleave + Viterbi + derandomize
              ->  decoded 134-byte OPV frames  ->  Interlocutor
```

`opv-decode` is the fabric-demod-split decode harness: the PL resolves
parity/polarity and emits soft metrics, so the A53 does decode only. Input
formats (see `opv-decode -h` / source): raw int16 soft (`default`),
channel-tagged multiplexed records (`-m N`), or the framed 3-bit
`m_axis_soft_bit` stream from `frame_sync_detector_soft` (`-3`, the deployment
seam). `-r` emits the decoded frame bytes; route those to Interlocutor exactly
as the LibreSDR/pluto_msk deployments do.

## Build

Built and shipped by the top-level dogu Makefile (`make cross && make deploy`).
It is dependency-light (libstdc++/libm/libc only — no libiio), so it does **not**
use dogu's libiio sysroot. Standalone:

```sh
make            # host build (development / the self-test golden)
make cross      # aarch64 (submodule TARGET=a53)
make clean
```

## Test

From the dogu root, after `make cross && make deploy`:

```sh
make test-frame-decoder      # host-golden vs on-target decode, asserts they match
make test-frame-decoder TEST_FRAMES=100
```
