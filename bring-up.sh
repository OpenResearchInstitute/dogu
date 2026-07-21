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
#   --- v6 map (VERSION 0x00060000+): MLSE sym-lock detector + CFO + timing ---
#   NOTE: 0x010-0x020 LPF_* and 0x068-0x094 Costas telemetry are RETIRED in the
#   MLSE demod (registers exist; loops do not). Written only to match the TB.
#   0x0A0 SYM_LOCK_STATUS(RO)  0x0A4 SYM_LOCK_THRESH(pct)
#   0x0A8 SYM_UNLOCK_THRESH(pct)  0x0AC SYM_LOCK_WINDOW(log2)
#   0x0B0 CFO_STATE(RO: 0=IDLE 1=SEARCH 2=CORRECTING 3=HELD 4=LOST)
#   0x0B4 CFO_ESTIMATE(RO, Hz)  0x0B8 CFO_CTRL(b0 auto, acq/trk shifts)
#   0x0BC CFO_MANUAL(Hz)        0x0C0 CFO_QUALITY(RO)
#   0x0C4 TIM_ALPHA(Q16)  0x0C8 TIM_BETA(Q24)  0x0CC SYM_CLK_OFFSET(RO)

# --- demod values to write ----------------------------------------------------
# FREQ words: VERIFIED = -/+13550 Hz at 625 ksps (32-bit NCO). Compare against
# the readback the script prints — if the live default already matches, these
# are redundant-but-harmless; if not, they fix it.
DM_FREQ_F1=0xFA732DF5         # lower tone, -13550 Hz
DM_FREQ_F2=0x058CD20B         # upper tone, +13550 Hz

# LPF registers: DEAD in the MLSE demod (Costas-era; wired to nothing).
# Values below are the TB's, written solely so hardware config == TB config
# byte-for-byte. Do NOT sweep these; the 10-anomaly campaign already proved
# they change nothing.
DM_P_GAIN=0x000033
DM_I_GAIN=0x000007
DM_ALPHA=0x000000             # TB value
DM_P_SHIFT=0x02               # TB value
DM_I_SHIFT=0x0C               # TB value

# rx_invert (CONTROL bit0): 0 = normal soft-bit polarity. If sym lock comes up
# but frames stay at 0, set this to 0x1 — the "I/Q backwards" one-bit toggle.
DM_CONTROL=0x00000000

# symbol-lock detector params (reset 128/8, matched to TB)
DM_SYM_LOCK_COUNT=0x00000080   # 128  window
DM_SYM_LOCK_THR=0x00000008     #   8  threshold

# --- v6 control plane: match TB writes 1130-1202 exactly. Reset defaults
# mirror these values, but bring-up depends on NO reset defaults (doctrine).
DM_GAIN_MANUAL=0x00000400      # 1.000 Q6.10 (TB)
DM_RX_DISCARD=0x00000000       # 0, not 0x18 (TB)
DM_SL_LOCK_PCT=0x00000019      # 25 pct (C++ LOCK_THRESH 0.25, verbatim)
DM_SL_UNLOCK_PCT=0x00000032    # 50 pct (C++ UNLOCK_THRESH 0.50, verbatim)
DM_SL_WINDOW=0x00000006        # log2 -> 64 symbols (TB)
DM_CFO_CTRL=0x00060A01         # acq_shift 6, trk_shift 10, b0 auto=1 (TB)
DM_CFO_MANUAL=0x00000000       # 0 Hz (TB)
DM_TIM_ALPHA=0x00000148        # 328 Q16 (C++ 0.005)
DM_TIM_BETA=0x000000A8         # 168 Q24 (C++ 1e-5)

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
        0x00060000) echo "      VERSION 0x00060000 — v6 map confirmed (MLSE + CFO AFC + timing loop)." ;;
        0x00050000) echo "      *** WARNING: VERSION 0x00050000 = pre-CFO bitstream (no AFC). ***" >&2 ;;
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
    # --- v6 map (VERSION 0x00060000+) ---
    D_GAIN_MANUAL=$((DEMOD_BASE + 0x030)); D_RX_DISCARD=$((DEMOD_BASE + 0x064))
    D_SL_STATUS=$((DEMOD_BASE + 0x0A0));   D_SL_LOCK=$((DEMOD_BASE + 0x0A4))
    D_SL_UNLOCK=$((DEMOD_BASE + 0x0A8));   D_SL_WINDOW=$((DEMOD_BASE + 0x0AC))
    D_CFO_STATE=$((DEMOD_BASE + 0x0B0));   D_CFO_EST=$((DEMOD_BASE + 0x0B4))
    D_CFO_CTRL=$((DEMOD_BASE + 0x0B8));    D_CFO_MANUAL=$((DEMOD_BASE + 0x0BC))
    D_CFO_QUAL=$((DEMOD_BASE + 0x0C0))
    D_TIM_ALPHA=$((DEMOD_BASE + 0x0C4));   D_TIM_BETA=$((DEMOD_BASE + 0x0C8))
    D_SYM_CLK_OFF=$((DEMOD_BASE + 0x0CC))

    echo "[6/7] demod live values BEFORE write (the real reset defaults):"
    printf "      FREQ_F1=%s FREQ_F2=%s\n" "$(devmem $D_FREQ_F1)" "$(devmem $D_FREQ_F2)"
    printf "      P_GAIN =%s I_GAIN =%s ALPHA=%s\n" "$(devmem $D_P_GAIN)" "$(devmem $D_I_GAIN)" "$(devmem $D_ALPHA)"
    printf "      P_SHIFT=%s I_SHIFT=%s\n" "$(devmem $D_P_SHIFT)" "$(devmem $D_I_SHIFT)"
    printf "      SYM_CNT=%s SYM_THR=%s\n" "$(devmem $D_SYM_CNT)" "$(devmem $D_SYM_THR)"

    echo "[7/7] writing demod config (TB order, TB values: tb_haifuraiya_channelizer_axi 1130-1202)"
    # TB order: hold DEMOD_INIT=1 through config, release last.
    devmem $D_DEMOD_INIT 32 0x00000001
    devmem $D_CONTROL 32 $DM_CONTROL
    devmem $D_FREQ_F1 32 $DM_FREQ_F1
    devmem $D_FREQ_F2 32 $DM_FREQ_F2
    # dead LPF quintet — TB parity only (see map note)
    devmem $D_P_GAIN  32 $DM_P_GAIN
    devmem $D_I_GAIN  32 $DM_I_GAIN
    devmem $D_ALPHA   32 $DM_ALPHA
    devmem $D_P_SHIFT 32 $DM_P_SHIFT
    devmem $D_I_SHIFT 32 $DM_I_SHIFT
    devmem $D_SYM_CNT 32 $DM_SYM_LOCK_COUNT
    devmem $D_SYM_THR 32 $DM_SYM_LOCK_THR
    devmem $D_GAIN_MANUAL 32 $DM_GAIN_MANUAL
    devmem $D_RX_DISCARD  32 $DM_RX_DISCARD
    # MLSE symbol-lock detector (percent thresholds + window)
    devmem $D_SL_LOCK   32 $DM_SL_LOCK_PCT
    devmem $D_SL_UNLOCK 32 $DM_SL_UNLOCK_PCT
    devmem $D_SL_WINDOW 32 $DM_SL_WINDOW
    # CFO AFC: auto mode armed, manual word zero
    devmem $D_CFO_CTRL   32 $DM_CFO_CTRL
    devmem $D_CFO_MANUAL 32 $DM_CFO_MANUAL
    # symbol timing loop
    devmem $D_TIM_ALPHA 32 $DM_TIM_ALPHA
    devmem $D_TIM_BETA  32 $DM_TIM_BETA
    # soft quantizer + frame-sync thresholds
    devmem $D_QUANT_1 32 $DM_QUANT_THR_1
    devmem $D_QUANT_2 32 $DM_QUANT_THR_2
    devmem $D_QUANT_3 32 $DM_QUANT_THR_3
    devmem $D_FS_HUNT   32 $DM_FS_HUNT_THR
    devmem $D_FS_VERIFY 32 $DM_FS_VERIFY_THR
    # loop control (rx_enable=1, not frozen/zeroed)
    devmem $D_LOOP_CTRL 32 $DM_LOOP_CTRL
    # release onto the live channel output — the TB's final config act
    devmem $D_DEMOD_INIT 32 0x00000000
    echo "      done. quant_thr=$DM_QUANT_THR_1/$DM_QUANT_THR_2/$DM_QUANT_THR_3  loop_ctrl=$DM_LOOP_CTRL"
    echo "      config readback (armed state into the transcript):"
    printf "      CFO_CTRL=%s (expect $DM_CFO_CTRL)  CFO_STATE=%s (expect 0x0/0x1 pre-carrier)\n" \
        "$(devmem $(printf '0x%X' $D_CFO_CTRL))" "$(devmem $(printf '0x%X' $D_CFO_STATE))"
    printf "      CFO_EST=%s  CFO_QUAL=%s  TIM_ALPHA=%s  TIM_BETA=%s\n" \
        "$(devmem $(printf '0x%X' $D_CFO_EST))" "$(devmem $(printf '0x%X' $D_CFO_QUAL))" \
        "$(devmem $(printf '0x%X' $D_TIM_ALPHA))" "$(devmem $(printf '0x%X' $D_TIM_BETA))"
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
    echo "2) Demod lock (STATUS b0=fsync; b1/b2 are retired Costas bits — ignore) + frames:"
    echo "   while true; do printf 'STATUS='; devmem $(printf '0x%X' $D_STATUS); printf ' FRAMES='; devmem $(printf '0x%X' $D_FRAMES); echo; sleep 0.3; done"
    echo ""
    echo "3) CFO AFC watch (STATE 0=IDLE 1=SEARCH 2=CORRECTING 3=HELD 4=LOST; EST in Hz):"
    echo "   while true; do printf 'CFO_STATE='; devmem $(printf '0x%X' $D_CFO_STATE); printf ' EST='; devmem $(printf '0x%X' $D_CFO_EST); printf ' QUAL='; devmem $(printf '0x%X' $D_CFO_QUAL); echo; sleep 0.2; done"
    echo ""
    echo "4) Symbol lock (measured, 0x0A0) + symbol clock offset (Q24, 0x0CC):"
    echo "   while true; do printf 'SYM_LOCK='; devmem $(printf '0x%X' $D_SL_STATUS); printf ' CLK_OFF='; devmem $(printf '0x%X' $D_SYM_CLK_OFF); echo; sleep 0.3; done"
    echo ""
    echo "5) Re-init the demod (recover from lock loss — NO bitstream reload):"
    echo "   devmem $(printf '0x%X' $D_DEMOD_INIT) 32 1; devmem $(printf '0x%X' $D_DEMOD_INIT) 32 0"
    echo "   # or via rx_enable:  devmem $(printf '0x%X' $D_LOOP_CTRL) 32 0; devmem $(printf '0x%X' $D_LOOP_CTRL) 32 4"
    echo ""
    echo "6) fs_hunt=$DM_FS_HUNT_THR fs_verify=$DM_FS_VERIFY_THR  quant=$DM_QUANT_THR_1/$DM_QUANT_THR_2/$DM_QUANT_THR_3"
    echo "   Quant threshold tuning (LIVE, iterative — soft bin edges to decoder):"
    echo "   # NOTE: CST_IQ_DELTA (0x094) is a Costas cal tap; may be undriven in MLSE."
    echo "   # Gauge mean|soft| from frame_decoder soft dumps instead, thr = mean*{1,2,3}/3.5."
    echo "   devmem $(printf '0x%X' $D_QUANT_1) 32 <t1>; devmem $(printf '0x%X' $D_QUANT_2) 32 <t2>; devmem $(printf '0x%X' $D_QUANT_3) 32 <t3>"
    echo "7) Symbol-lock tuning (no direct readout — sweep, watch frames react):"
    echo "   # carrier must already be locked (STATUS b1/b2=1). Then sweep the threshold:"
    echo "   for t in 4 8 12 16 24; do devmem $(printf '0x%X' $D_SYM_THR) 32 \$t; sleep 1; \\"
    echo "     printf 'thr=%2d STATUS=' \$t; devmem $(printf '0x%X' $D_STATUS); \\"
    echo "     printf ' FRAMES='; devmem $(printf '0x%X' $D_FRAMES); echo; done"
fi
