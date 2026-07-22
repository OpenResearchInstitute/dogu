# Lab note: OTA soft-bit capture & decode workflow (ZCU102 / Haifuraiya)

Written 2026-07-21, the day of first OTA decode. Two SSH consoles to the
target (ssh root@10.73.1.16), TX = Pluto via Dialogus/Interlocutor,
continuous Opus frames (constant rate regardless of speech -- silence does
NOT gap the stream).

## Prerequisites (once per boot)

1. Bitstream identity:  `devmem 0x84A80000`  -> expect 0x00060000 (v6 map).
2. Run bring-up (TB-order config; CFO auto arms from reset):
   `/home/root/bring-up.sh <profile>` -- transcript shows [7/7] readback:
   CFO_CTRL=0x00060A01, CFO_STATE 0x0/0x1 pre-carrier.
3. dma_listen must be the 16-BIT-AWARE build (banner says
   "16-bit DMA words (low byte data)"). If it says "1 byte each", it is
   the old tool and every capture will look zero-stuffed -- redeploy dogu.

## Console 1 -- the consumer (START THIS FIRST, ALWAYS)

    killall dma_listen 2>/dev/null; sleep 1
    pgrep -a dma_listen        # must print NOTHING (see Gotchas)
    ./dma_listen -n 1072 -t 5000 -k -w /tmp/fresh.bin &

- -n 1072 : 1072 samples x 4 B = 4288 DMA bytes = 2144 softs = ONE frame
  per refill (integer frames; no mid-frame warnings).
- -t 5000 -k : 5 s refill timeout, retry forever -- survives dead air and
  wedges without exiting. NEVER run without -k.
- -w file : de-stuffed capture, 1 byte/soft, opv-decode -3 layout.
- Healthy output: "2144 B = 1.00 frames", hist ~50/50 in bins 0 and 7,
  occasional small middle-bin counts (real channel softness), no oor.

## Console 2 -- revive + watch (pulse ONLY after console 1 is running)

    devmem 0x84A8005C 32 1; devmem 0x84A8005C 32 0     # DEMOD_INIT pulse

The pulse is the wedge defibrillator. Consumer first, pulse second --
reviving into an unposted buffer re-wedges within ~1 frame.

Watch loops (pick one):

    # decode-chain health: frames + capture bytes should climb in lockstep
    while true; do printf 'FRAMES='; devmem 0x84A80044; printf '  BYTES='; \
      wc -c < /tmp/fresh.bin; sleep 0.5; done

    # CFO AFC: STATE 0=IDLE 1=SEARCH 2=CORRECTING 3=HELD 4=LOST(warm)
    while true; do printf 'CFO_STATE='; devmem 0x84A800B0; printf ' EST='; \
      devmem 0x84A800B4; printf ' QUAL='; devmem 0x84A800C0; sleep 0.2; done

    # timing-loop ppm logger (0x0CC, signed Q24; ppm = q24*1e6/193478/1000)
    while true; do printf '%s %s\n' "$(date +%s.%N)" \
      "$(devmem 0x84A800CC)"; sleep 1; done | tee /tmp/ppm_log.txt

Healthy: QUAL breathing 6k-22k, HELD, EST = the real LO offset (it will
drift over hours -- thermal; the AFC tracks it). ALL VALUES BIT-FROZEN =
the wedge: nothing is wrong with your procedure; pulse again (console 1
still running) and note the time.

## Decoding a capture

    ./opv-decode -3 /tmp/fresh.bin            # WRONG -- hangs (reads stdin)
    ./opv-decode -3    < /tmp/fresh.bin | head -40      # metrics only
    ./opv-decode -3 -r < /tmp/fresh.bin > payload.bin   # + payload bytes
    ./opv-decode -3 -q < /tmp/fresh.bin                 # summary line only

- metric 0 (perfect) = clean frame. Gold signature on (re)acquisition:
  7 then a run of 0s.
- Pockets of graded bad metrics every ~0.76 s = symbol-clock slips from
  the ~11.5 ppm free-running-oscillator offset (see WP_SYMBOL_CLOCK_SLIP).
  Expected ~20-22% of frames unreferenced. NOT a plumbing fault.
- Payload bytes on the terminal look like binary garbage: that IS the
  voice (Opus). Redirect -r to a file.

## Archiving (provenance or it didn't happen)

    cp /tmp/fresh.bin  ~/captures-staging/ota_$(date +%Y%m%d_%H%M).bin
    # commit to haifuraiya/captures/ with: date, bitstream VERSION,
    # dma_listen build (16-bit), -n value, TX source, CFO EST at capture,
    # and the decode summary line. md5sum in the commit message.

## Gotchas (each one cost us time on 2026-07-21)

- **Consumer first, then pulse.** Every time, both directions of restart.
- **Respawner hydras:** a `while true ... dma_listen` wrapper loop lives in
  the shell that launched it and revives children forever. Find with
  `jobs` in EVERY open SSH window (kill %N), or kill the parent:
  `grep PPid /proc/$(pgrep dma_listen | head -1)/status` then kill that
  PID. Verify silence with pgrep three times over ~6 s.
- **opv-decode file argument hangs** -- it reads stdin only. Use `<`.
- **Old capture files from the 1-byte-era tool** decode only after
  de-stuffing (`python3 -c "import sys;d=open(sys.argv[1],'rb').read();\
  open(sys.argv[2],'wb').write(d[0::2])" old.bin fixed.bin`).
- **mv on a live capture** doesn't redirect the writer (fd follows the
  file); kill/restart the consumer to switch output files.
- FRAMES (0x044) counts frames SYNC'D AND FORWARDED by fabric; decode
  happens on the A53. It is not a decode counter.

## Reference-port survey (2026-07-21, for the slip WP's kill test)

- ADRV9002 eval (this bench): has an external reference PORT; NO external
  oscillator fitted -- free-running.
- Pluto (TX): no external source connected -- free-running.
- Both free-running is consistent with the measured 11.5 ppm symbol-clock
  offset and the 0.756 s slip metronome.
- ADRV9009 (next bench) carries the ADI-recommended external oscillator --
  candidate donor/reference source for the common-10-MHz test. Consult
  Paul (KB5MU) for safe interconnect before cabling anything.
