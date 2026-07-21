# WP: Soft-bit stream format contract (the 16-bit word seam)

**Status:** root-caused and consumer-fixed 2026-07-21 (OTA capture forensics).
Remaining work: contract documentation in-tree, regression gates, optional
bandwidth optimization. No RTL logic change required — the fabric is correct.

## What happened

The frame_sync detector emits one 3-bit soft value per coded bit on
m_axis_soft_bit. To board the S2MM DMA, `axis_softbit_widen`
(syn/zcu102_with_adrv9001/axis_softbit_widen.vhd, OUT_WIDTH := 16) zero-extends
each soft to a 16-bit word — required because axi_dmac's minimum source width
is 16. In DRAM, little-endian, every soft therefore occupies TWO bytes:
[soft][0x00].

`dma_listen` walked the buffer at ONE byte per soft. Result: phantom period-2
zero-stuffing (75/25 histogram), every captured "frame" holding half a
codeword, uniform Viterbi failure (~2044) on every frame, frame sync unaffected.
Six suspects were eliminated by measurement before the format confessed:
polarity (bench experiment), 0..7 mapping (source diff, both sides), byte
alignment (exhaustive 2144-offset search), deinterleave/reversal permutations,
and both generator-polynomial vintages (0x67/0x76 and pre-correction
0x4F/0x6D).

The original calibration error: PARTIAL_XFER_LEN "4288 = 2 frames" was read as
bytes. The register counts 16-bit beats. 4288 beats = 2 frames is correct;
4288 BYTES is one frame. The wrong units fossilized into FRAME_SOFT_BYTES.

Why sim never caught it: tb_haifuraiya_channelizer_axi taps sb_tdata (the
3-bit stream) UPSTREAM of the widen — seam_chan_out.txt is one value per line
and decodes clean (verified 2026-07-21: balanced 46.7/53.3, and the de-stuffed
OTA capture decodes 803/1029 frames at metric 0, gold signature 7,0,0,0,0,0).
The widen + DMA + reader stages had no coverage anywhere.

## The contract (normative)

- m_axis_soft_bit (3-bit AXIS): one soft 0..7 per beat, 2144 beats/frame,
  tlast on the final beat. Value 0 = confident logical 0, 7 = confident 1
  (matches opv_demod.hpp decode_soft3: cost sg for expected-0, 7-sg for
  expected-1).
- After axis_softbit_widen / DMA / DRAM: one soft per LITTLE-ENDIAN 16-bit
  word, low byte = soft, high byte = 0x00. 4288 bytes per frame.
- Capture files for opv-decode -3: one byte per soft, 2144 bytes/frame
  (consumers de-stuff; the file format does NOT carry the DMA width).

## Work items

1. **dma_listen 16-bit ingest** — DONE (patch delivered 2026-07-21): strides
   16-bit words, takes low byte, VERIFIES high byte == 0 (violations counted
   as out-of-range with the offending byte logged — the contract is now
   self-checking at runtime), writes de-stuffed 1-byte/soft capture files.
   Deploy via dogu make cross/deploy; verify: live histogram ~50/50 and
   opv-decode -3 streaming metric 0 on fresh captures.
2. **Contract doc in-tree**: this file, plus a pointer comment beside
   OUT_WIDTH in axis_softbit_widen.vhd and in dogu/tools/dma_listen/README.
3. **Regression gates**:
   a. SIM: seam_chan_out.txt -> opv-decode -3 -> assert metrics == 0 as a
      standing pass/fail step in the sim flow (the checkpoint existed since
      the tb was written; it was never cashed — cash it every run).
   b. HW: dma_listen's high-byte check makes format drift self-announcing;
      bring-up or a smoke script should run one short capture and assert
      oor_count == 0 and histogram balance within 45-55%.
4. **Optional, later**: pack 2 softs per 16-bit word (or 4 per 32) in the
   widen to halve DMA bandwidth. Contract change — requires simultaneous
   dma_listen update and a version/flag so mixed deployments fail loudly,
   not silently. Do not do this casually; today's bug is what a silent
   format change looks like.

## Related but separate

- The forwarding-path wedge (stalls with ~1 true frame of slack when no DMA
  descriptor is posted; revivable only by DEMOD_INIT) is a DIFFERENT defect
  with its own WP: fabric-side drop-policy FIFO + DROPPED counter +
  FIFO_HIGH_WATER register; demod never sees tready. Note the slack quantum
  restates as 4288 DMA bytes = ONE true frame under this contract.
- Requirements logged 2026-07-21, both testable in that WP's acceptance
  campaign: dead air shall never cause radio issues; a slow, stopped, or
  absent consumer shall never affect demodulation (payload may drop, counted;
  lock may not).
