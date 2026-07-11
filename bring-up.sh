#!/bin/sh
# bring-up.sh — Haifuraiya LVDS bring-up + channelizer/demod register config
# Open Research Institute
#
# Usage:  /home/root/bring-up.sh <profile-basename> [output_shift]
#
# -----------------------------------------------------------------------------
# TWO AXI-Lite slaves, TWO separate bases. Offsets collide (both start at 0x000)
# but mean different things. NEVER write a demod register using the channelizer
# base. For example, 0x84A70010 is the channelizer's DROPPED reg, not LPF_P_GAIN.
#
#   CHANNELIZER  base 0x84A70000  (s_axi_ctrl)   — VERIFIED
#   DEMOD        base 0x84A80000  (s_axi_demod)  — from Vivado address editor
#
# The LPF shift sweep (Section 2), try and start mid-bracket, bisect on lock
# Everything else is verified. 
# -----------------------------------------------------------------------------

set -e

# =============================================================================
# SECTION 0 — Parameters
# =============================================================================
PROFILE=${1:?usage: $0 <profile-basename> [output_shift]}
PROFILE_DIR=${PROFILE_DIR:-/home/root}
STREAM_FILE="${PROFILE_DIR}/${PROFILE}.bin"
PROFILE_FILE="${PROFILE_DIR}/${PROFILE}.json"
IIO_PHY=/sys/bus/iio/devices/iio:device1

RX1_LO_HZ=5600000000          # W2 RX, 5.6 GHz
TX1_LO_HZ=5800000000          # W2 TX, 5.8 GHz

# --- OUTPUT_SHIFT -------------------------------------------------------------
# Sets channel amplitude INTO the demod. Costas gain is proportional to
# amplitude^2, so this is a LOCK variable, not just a display knob.
#   reset default = 16
#   TB OPV->demod path uses 14 ("match the board"; 4x more amp than 16)
#   old plumbing-era script default was 4 (raw-ADC era, before the demod path)
# Defaulting to the TB-validated demod value (14). Override as arg 2 to sweep.
# NOTE: this is a change from the old default of 4 — flagged on purpose.
OUTPUT_SHIFT=${2:-14}

# =============================================================================
# SECTION 1 — CHANNELIZER registers   (base 0x84A70000, VERIFIED)
# =============================================================================
CH_CONTROL=0x84A70004         # bit0 soft_reset, bit1 enable
CH_STATUS=0x84A70008          # write 0x06 to clear stickies
CH_FRAME_COUNT=0x84A7000C
CH_DROPPED=0x84A70010
CH_OUTPUT_SHIFT=0x84A70014
CH_ALPHA1=0x84A70018          # EMA power-detector alpha (reset 4096 = correct)
CH_ALPHA2=0x84A7001C          # EMA power-detector alpha (reset 64 is TOO SLOW -> write 256)
# per-channel power array: 0x84A70100 + 4*k
CH_POW_CH5=0x84A70114         # input-bin-5 mirror
CH_POW_CH59=0x84A701EC        # input-bin-5 target (after commutator reversal: 64-5)

# =============================================================================
# SECTION 2 — DEMOD registers   (base = FILL IN, s_axi_demod)
# =============================================================================
# Get this from the Vivado address editor. Leave at 0 to skip demod config
# entirely (channelizer still comes up).
DEMOD_BASE=0x84A80000

# Offset map (verified from demod axi register file):
#   0x000 VERSION    0x004 CONTROL(bit0 rx_invert)
#   0x008 FREQ_F1    0x00C FREQ_F2
#   0x010 P_GAIN     0x014 I_GAIN      0x018 ALPHA
#   0x01C P_SHIFT    0x020 I_SHIFT
#   0x024 SYM_CNT    0x028 SYM_THR
#   0x040 STATUS(b0 fsync, b1 cst_f1, b2 cst_f2)    0x044 FRAMES_RX
#   0x048 FS_HUNT_THR  0x04C FS_VERIFY_THR
#   0x050/054/058 QUANT_THR_1/2/3   (NOW LIVE: wired to frame_sync quantizer)
#   --- expanded control plane (VERSION 0x00050000+) ---
#   0x05C DEMOD_INIT(b0 rx_init)  0x060 LOOP_CTRL(b0 freeze,b1 zero,b2 rx_enable)
#   0x064 RX_SAMPLE_DISCARD[7:0]
#   0x068 F1_NCO_ADJUST  0x06C F2_NCO_ADJUST     (RO: carrier drift)
#   0x070 F1_ERROR       0x074 F2_ERROR          (RO)
#   0x078 LPF_ACCUM_F1   0x07C LPF_ACCUM_F2      (RO)
#   0x080 CST_LOCKTIME_F1 0x084 CST_LOCKTIME_F2  (RO)
#   0x088 LOCK_STATUS(b0 lk_f1,b1 lk_f2,b2 unlk_f1,b3 unlk_f2)
#   0x08C CST_ACC_I_F1  0x090 CST_ACC_Q_F1  0x094 CST_IQ_DELTA_F1 (RO)

# --- demod values to write ----------------------------------------------------
# FREQ words: VERIFIED = -/+13550 Hz at 625 ksps (32-bit NCO). Compare against
# the readback the script prints — if the live default already matches, these
# are redundant-but-harmless; if not, they fix it.
DM_FREQ_F1=0xFA732DF5         # lower tone, -13550 Hz
DM_FREQ_F2=0x058CD20B         # upper tone, +13550 Hz

# LPF gains: STARTING POINTS TO SWEEP — NOT derived. With the mantissa pinned at
# max, effective gain ~= 2^(23 - shift), so +1 to a shift halves the gain.
# Brackets already tried: P/I shift 13/16 (hot), 20/29 (cold) — both failed.
# Start mid, hold (I_SHIFT - P_SHIFT) = 3 so only bandwidth moves, then bisect.
DM_P_GAIN=0x7FFFFF
DM_I_GAIN=0x7FFFFF
DM_ALPHA=0x000000            # restore nonzero
DM_P_SHIFT=0x11              # 17   <-- SWEEP
DM_I_SHIFT=0x14              # 20   <-- SWEEP (keep = P_SHIFT + 3)

# rx_invert (CONTROL bit0): 0 = normal soft-bit polarity. If cst_lock comes up
# but frames stay at 0, set this to 0x1 — the "I/Q backwards" one-bit toggle.
DM_CONTROL=0x00000001

# symbol-lock detector params (reset 128/8, matched to TB)
DM_SYM_LOCK_COUNT=0x00000080   # 128  window
DM_SYM_LOCK_THR=0x00000008     #   8  threshold

# --- quant thresholds: 3-bit soft bin edges. GOLDEN RULE (opv_demod.hpp
# FrameDecoder, verified against the fabric quantize()): thr_k = mean|soft|*k/3.5,
# i.e. a 1:2:3 spacing. Sim measured mean|soft| = 17297 at 8 dB WITH THE
# NORMALIZER ACTIVE (GAIN_TARGET=16000, rail 32768) -> 4942/9884/14826.
#
# ASSUMPTION: the per-channel normalizer is ACTIVE on hardware, so the hw soft
# scale matches sim. If it is NOT (raw channel amplitude), the hw soft rails
# ~10x lower and these are ~10x too big. To retune on hardware with no rebuild:
# read CST_IQ_DELTA (0x094) while locked for real mean|soft|, then
# thr = round(mean|soft| * {1,2,3}/3.5).  (Old 540/1620/2700 = stale pre-normalizer.)
DM_QUANT_THR_1=0x0000134E    # 4942
DM_QUANT_THR_2=0x0000269C    # 9884
DM_QUANT_THR_3=0x000039EA    # 14826

# --- frame-sync thresholds: NOW PERCENTS (0..100), NOT absolute counts. The
# frame_sync detector switched to a normalized GLRT/CFAR test:
#     100*corr_prev >= PCT*energy_prev
# HUNT 85 = 4.2 sigma vs random data; VERIFY 70. The old absolute 115000/68000
# values are STALE and are nonsense as a percent -- writing them would make HUNT
# unreachable (energy never 1150x corr) and effectively disable acquisition.
# (Also fixes the old M_FS_VERIFY_THR typo: the "D" was missing, so the verify
#  write got an empty value.)
DM_FS_HUNT_THR=0x00000055    # 85 percent
DM_FS_VERIFY_THR=0x00000046  # 70 percent

# --- loop control: b0 lpf_freeze, b1 lpf_zero, b2 rx_enable. 0x4 = reset default
# (enabled, not frozen). rx_enable doubles as a re-init lever (clear 0x0 -> set 0x4).
DM_LOOP_CTRL=0x00000004

# =============================================================================
# SECTION 3 — Profile + stream load
# =============================================================================
[ -f "$STREAM_FILE" ]  || { echo "missing $STREAM_FILE"  >&2; exit 1; }
[ -f "$PROFILE_FILE" ] || { echo "missing $PROFILE_FILE" >&2; exit 1; }
echo "[1/7] stream + profile load: $PROFILE"
cat "$STREAM_FILE"  > "$IIO_PHY/stream_config"
cat "$PROFILE_FILE" > "$IIO_PHY/profile_config"

# =============================================================================
# SECTION 4 — LO tune (W2)
# =============================================================================
echo "[2/7] LO tune: RX1=$RX1_LO_HZ  TX1=$TX1_LO_HZ"
iio_attr -c adrv9002-phy -o altvoltage0 frequency $RX1_LO_HZ >/dev/null
iio_attr -c adrv9002-phy -o altvoltage2 frequency $TX1_LO_HZ >/dev/null

# =============================================================================
# SECTION 5 — Calibrations
# =============================================================================
echo "[3/7] calibrations (safe 1T1R sequence)"
/home/root/oriinit-cli run-calibrations

# =============================================================================
# SECTION 6 — Channelizer enable
# =============================================================================
echo "[4/7] channelizer: clear stickies, OUTPUT_SHIFT=$OUTPUT_SHIFT, enable"
devmem $CH_STATUS       32 0x00000006
# EMA power-detector alphas. ALPHA2 reset (64) gives 32.8 ms AGC settle, which
# BLOWS the 40 ms preamble budget; 256 gives 8.2 ms. Write both explicitly.
devmem $CH_ALPHA1       32 0x00001000   # 4096
devmem $CH_ALPHA2       32 0x00000100   #  256
devmem $CH_OUTPUT_SHIFT 32 $(printf "0x%08X" $OUTPUT_SHIFT)
devmem $CH_CONTROL      32 0x00000002
sleep 1; C1=$(devmem $CH_FRAME_COUNT 32)
sleep 1; C2=$(devmem $CH_FRAME_COUNT 32)
if [ "$C1" = "$C2" ]; then
    echo "  WARNING: FRAME_COUNT stuck at $C1" >&2
else
    echo "  frame_count live: $C1 -> $C2"
fi

# brief AFE level report (meaningful only with RX live; report-only)
echo "  AFE: ensm=$(cat $IIO_PHY/in_voltage0_ensm_mode 2>/dev/null) " \
     "gain=$(cat $IIO_PHY/in_voltage0_hardwaregain 2>/dev/null)" \
     "dec_pwr=$(cat $IIO_PHY/in_voltage0_decimated_power 2>/dev/null)dB" \
     "rssi=$(cat $IIO_PHY/in_voltage0_rssi 2>/dev/null)dB  (dB below FS)"

# =============================================================================
# SECTION 7 — Demod config   (only if DEMOD_BASE set AND VERSION verified)
# =============================================================================
if [ "$DEMOD_BASE" = "0x00000000" ]; then
    echo "[5/7] DEMOD_BASE not set — skipping demod config."
    echo "      Fill DEMOD_BASE (Section 2) from the Vivado address editor, re-run."
    D_STATUS=""; D_FRAMES=""
else
    # --- verify the base by reading VERSION (don't trust a guessed base) ---
    set +e
    DVER=$(devmem $((DEMOD_BASE + 0x000)) 2>/dev/null); rc=$?
    set -e
    if [ $rc -ne 0 ] || [ -z "$DVER" ] \
       || [ "$DVER" = "0x00010000" ] || [ "$DVER" = "0x00000000" ] || [ "$DVER" = "0xFFFFFFFF" ]; then
        echo "[5/7] ERROR: demod VERSION probe = '${DVER:-<none>}' (rc=$rc)." >&2
        echo "      0x00010000 = channelizer (WRONG base); 0/F = unmapped. Check DEMOD_BASE." >&2
        exit 1
    fi
    echo "[5/7] demod VERSION = $DVER  (looks like demod — base OK)"
    case "$DVER" in
        0x00050000) echo "      VERSION 0x00050000 — expanded map confirmed (new bitstream)." ;;
        0x00040000) echo "      *** WARNING: VERSION 0x00040000 = PRE-EXPANSION bitstream. ***" >&2
                    echo "          New control plane + quant_thr wiring are NOT present —" >&2
                    echo "          you are likely on the stale ipshared copy. Re-package + re-synth." >&2 ;;
        *)          echo "      NOTE: unexpected VERSION $DVER — proceeding; verify the map." ;;
    esac

    # resolve absolute addresses once
    D_CONTROL=$((DEMOD_BASE + 0x004))
    D_FREQ_F1=$((DEMOD_BASE + 0x008));  D_FREQ_F2=$((DEMOD_BASE + 0x00C))
    D_P_GAIN=$((DEMOD_BASE + 0x010));   D_I_GAIN=$((DEMOD_BASE + 0x014))
    D_ALPHA=$((DEMOD_BASE + 0x018))
    D_P_SHIFT=$((DEMOD_BASE + 0x01C));  D_I_SHIFT=$((DEMOD_BASE + 0x020))
    D_STATUS=$((DEMOD_BASE + 0x040));   D_FRAMES=$((DEMOD_BASE + 0x044))
    # --- expanded map (VERSION 0x00050000+) ---
    D_QUANT_1=$((DEMOD_BASE + 0x050));  D_QUANT_2=$((DEMOD_BASE + 0x054));  D_QUANT_3=$((DEMOD_BASE + 0x058))
    D_FS_HUNT=$((DEMOD_BASE + 0x048));  D_FS_VERIFY=$((DEMOD_BASE + 0x04C))
    D_DEMOD_INIT=$((DEMOD_BASE + 0x05C)); D_LOOP_CTRL=$((DEMOD_BASE + 0x060))
    D_F1_NCO=$((DEMOD_BASE + 0x068));   D_F2_NCO=$((DEMOD_BASE + 0x06C))
    D_F1_ERR=$((DEMOD_BASE + 0x070));   D_F2_ERR=$((DEMOD_BASE + 0x074))
    D_LPF_ACC_F1=$((DEMOD_BASE + 0x078)); D_LPF_ACC_F2=$((DEMOD_BASE + 0x07C))
    D_LOCKTIME_F1=$((DEMOD_BASE + 0x080)); D_LOCKTIME_F2=$((DEMOD_BASE + 0x084))
    D_LOCK_STATUS=$((DEMOD_BASE + 0x088))
    D_ACC_I=$((DEMOD_BASE + 0x08C));    D_ACC_Q=$((DEMOD_BASE + 0x090));    D_IQ_DELTA=$((DEMOD_BASE + 0x094))
    # symbol lock resolve + write (beside the others)
    D_SYM_CNT=$((DEMOD_BASE + 0x024));  D_SYM_THR=$((DEMOD_BASE + 0x028))

    echo "[6/7] demod live values BEFORE write (the real reset defaults):"
    printf "      FREQ_F1=%s FREQ_F2=%s\n" "$(devmem $D_FREQ_F1)" "$(devmem $D_FREQ_F2)"
    printf "      P_GAIN =%s I_GAIN =%s ALPHA=%s\n" "$(devmem $D_P_GAIN)" "$(devmem $D_I_GAIN)" "$(devmem $D_ALPHA)"
    printf "      P_SHIFT=%s I_SHIFT=%s\n" "$(devmem $D_P_SHIFT)" "$(devmem $D_I_SHIFT)"
    printf "      SYM_CNT=%s SYM_THR=%s\n" "$(devmem $D_SYM_CNT)" "$(devmem $D_SYM_THR)"

    echo "[7/7] writing demod config (FREQ verified; gains = sweep points)"
    devmem $D_FREQ_F1 32 $DM_FREQ_F1
    devmem $D_FREQ_F2 32 $DM_FREQ_F2
    devmem $D_P_GAIN  32 $DM_P_GAIN
    devmem $D_I_GAIN  32 $DM_I_GAIN
    devmem $D_ALPHA   32 $DM_ALPHA
    devmem $D_P_SHIFT 32 $DM_P_SHIFT
    devmem $D_I_SHIFT 32 $DM_I_SHIFT
    devmem $D_CONTROL 32 $DM_CONTROL

    # frame-sync hunt/verify thresholds — match the TB
    devmem $D_FS_HUNT   32 $DM_FS_HUNT_THR
    devmem $D_FS_VERIFY 32 $DM_FS_VERIFY_THR

    # symbol-lock detector params — match the TB
    devmem $D_SYM_CNT 32 $DM_SYM_LOCK_COUNT
    devmem $D_SYM_THR 32 $DM_SYM_LOCK_THR

    # quant thresholds — now reach the frame_sync soft quantizer
    devmem $D_QUANT_1 32 $DM_QUANT_THR_1
    devmem $D_QUANT_2 32 $DM_QUANT_THR_2
    devmem $D_QUANT_3 32 $DM_QUANT_THR_3
    # loop control (rx_enable=1, not frozen/zeroed)
    devmem $D_LOOP_CTRL 32 $DM_LOOP_CTRL
    echo "      done. P_SHIFT=$DM_P_SHIFT I_SHIFT=$DM_I_SHIFT ALPHA=$DM_ALPHA"
    echo "      quant_thr=$DM_QUANT_THR_1/$DM_QUANT_THR_2/$DM_QUANT_THR_3  loop_ctrl=$DM_LOOP_CTRL"

    # Clean re-init AFTER channelizer is up + demod configured: pulse DEMOD_INIT
    # (1 -> 0) so the carrier loops start fresh on the stable channel output. This
    # is the lever the channelizer soft-reset never reached. Back-to-back writes
    # hold init for several ms (thousands of clocks) — no fractional sleep needed.
    # On a pre-0x50000 bitstream 0x05C is unmapped; the writes are harmlessly ignored.
    devmem $D_DEMOD_INIT 32 0x00000001
    devmem $D_DEMOD_INIT 32 0x00000000
    echo "      demod re-init pulsed (DEMOD_INIT 1->0)."
fi

# =============================================================================
# Monitoring — copy-paste these while keying the OPV
# =============================================================================
echo ""
echo "bring-up complete."
echo ""
echo "1) Signal placement (key OPV on/off; ch59 should rise, ch5 stay low):"
echo "   while true; do printf 'ch59='; devmem $CH_POW_CH59; printf ' ch5='; devmem $CH_POW_CH5; echo; sleep 0.3; done"
if [ -n "$D_STATUS" ]; then
    echo ""
    echo "2) Demod lock (STATUS b0=fsync b1=cst_f1 b2=cst_f2) + frames:"
    echo "   while true; do printf 'STATUS='; devmem $(printf '0x%X' $D_STATUS); printf ' FRAMES='; devmem $(printf '0x%X' $D_FRAMES); echo; sleep 0.3; done"
    echo ""
    echo "3) Carrier drift — f1/f2 NCO adjust (wander apart = decoupled loops, the sim question):"
    echo "   while true; do printf 'f1_nco='; devmem $(printf '0x%X' $D_F1_NCO); printf ' f2_nco='; devmem $(printf '0x%X' $D_F2_NCO); echo; sleep 0.3; done"
    echo ""
    echo "4) Loop scoreboard (LOCK b0/1=lock_f1/2 b2/3=unlock_f1/2):"
    echo "   while true; do printf 'LOCK='; devmem $(printf '0x%X' $D_LOCK_STATUS); printf ' f1err='; devmem $(printf '0x%X' $D_F1_ERR); printf ' lpf1='; devmem $(printf '0x%X' $D_LPF_ACC_F1); printf ' lt1='; devmem $(printf '0x%X' $D_LOCKTIME_F1); echo; sleep 0.3; done"
    echo ""
    echo "5) Re-init the demod (recover from lock loss — NO bitstream reload):"
    echo "   devmem $(printf '0x%X' $D_DEMOD_INIT) 32 1; devmem $(printf '0x%X' $D_DEMOD_INIT) 32 0"
    echo "   # or via rx_enable:  devmem $(printf '0x%X' $D_LOOP_CTRL) 32 0; devmem $(printf '0x%X' $D_LOOP_CTRL) 32 4"
    echo ""
    echo "6) fs_hunt=$DM_FS_HUNT_THR fs_verify=$DM_FS_VERIFY_THR  quant=$DM_QUANT_THR_1/$DM_QUANT_THR_2/$DM_QUANT_THR_3"
    echo "   Quant threshold tuning (LIVE, iterative — soft bin edges to decoder):"
    echo "   devmem $(printf '0x%X' $D_IQ_DELTA)        # soft metric magnitude while locked (gauge the edges)"
    echo "   devmem $(printf '0x%X' $D_QUANT_1) 32 <t1>; devmem $(printf '0x%X' $D_QUANT_2) 32 <t2>; devmem $(printf '0x%X' $D_QUANT_3) 32 <t3>"
    echo "7) Symbol-lock tuning (no direct readout — sweep, watch frames react):"
    echo "   # carrier must already be locked (STATUS b1/b2=1). Then sweep the threshold:"
    echo "   for t in 4 8 12 16 24; do devmem $(printf '0x%X' $D_SYM_THR) 32 \$t; sleep 1; \\"
    echo "     printf 'thr=%2d STATUS=' \$t; devmem $(printf '0x%X' $D_STATUS); \\"
    echo "     printf ' FRAMES='; devmem $(printf '0x%X' $D_FRAMES); echo; done"
fi
