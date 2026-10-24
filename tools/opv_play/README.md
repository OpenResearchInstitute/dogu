# opv_play

Plays a stimulus file through the Haifuraiya receiver in the VCK190's fabric and
captures the soft bits -- the "memory loopback" (tier 1) test. Runs on the board as
root; needs `/dev/uio0` (the receiver's registers) and `/dev/dma_proxy_{tx,rx}`
(the dma-proxy module from the MDT repo's PetaLinux recipe).

```
opv_play -i stim.bin -o soft.bin [-f max_frames] [-q] [-v]
```

Host side (Python 3, NumPy; both scripts in haifuraiya/platforms/vck190/tools/):

```
python3 opv_stim_txt2bin.py opv_chan_stim.txt stim.bin      # text I Q -> packed int16 pairs
scp stim.bin root@10.73.1.17:/tmp/
ssh root@10.73.1.17 'opv_play -i /tmp/stim.bin -o /tmp/soft.bin -q'
scp root@10.73.1.17:/tmp/soft.bin .
python3 opv_soft_bin2txt.py soft.bin seam_chan_out.txt      # bytes -> one value per line
python3 ../../../sim/grade_payload_ber.py seam_chan_out.txt --stim-dir ../../../sim
```

Pass = the same per-frame table the 25-frame xsim run gives (25/25, flat).

The register bring-up is the simulation bench's Phase 2 sequence, register for
register (`configure_receiver()` in the source), so the board is configured the
way the acceptance run is. `-q` prints the readbacks (CFO estimate, symbol clock
offset in ppm, frames received, soft drops) -- the same signals watched in the
waveform.
