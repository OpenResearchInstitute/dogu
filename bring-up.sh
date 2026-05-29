#!/bin/sh
# bring-up.sh — single-command Haifuraiya LVDS bring-up
#
# Captures the working day-6 sequence as executable knowledge:
#   stream + profile dual-load → W2 LO retune → run-calibrations →
#   channelizer enable with proper OUTPUT_SHIFT
#
# Usage:
#   /home/root/bring-up.sh <profile-basename> [output_shift]
#
# Examples:
#   /home/root/bring-up.sh tes_0231_Haifuraiya_FDD_LVDS_20Msps_10MHz
#   /home/root/bring-up.sh tes_0231_Haifuraiya_FDD_LVDS_20Msps_10MHz 4
#   /home/root/bring-up.sh tes_0231_Haifuraiya_FDD_CMOS_1.92Msps_1.008MHz 2
#
# Profile basename: looks for ${name}.bin (stream) and ${name}.json (profile)
# in ${PROFILE_DIR} (default /home/root).
#
# After this script completes, /home/root/dma_listen should report
# MULTI-BIT (real ADC samples?) and the hex bytes should be varying values
# across the 16-bit signed range.

set -e

PROFILE=$1
OUTPUT_SHIFT=${2:-2}
PROFILE_DIR=${PROFILE_DIR:-/home/root}

IIO_PHY=/sys/bus/iio/devices/iio:device1
CH_CONTROL=0x84A70004
CH_STATUS=0x84A70008
CH_OUTPUT_SHIFT=0x84A70014
CH_FRAME_COUNT=0x84A7000C

RX1_LO_HZ=5600000000        # W2 RX, 5.6 GHz
TX1_LO_HZ=5800000000        # W2 TX, 5.8 GHz

# --- usage check -------------------------------------------------------------

if [ -z "$PROFILE" ]; then
    echo "Usage: $0 <profile-basename> [output_shift]"
    echo ""
    echo "Looks for <basename>.bin and <basename>.json in \$PROFILE_DIR"
    echo "(default: /home/root)."
    echo ""
    echo "Defaults: output_shift=2"
    echo ""
    echo "Examples:"
    echo "  $0 tes_0231_Haifuraiya_FDD_LVDS_20Msps_10MHz"
    echo "  $0 tes_0231_Haifuraiya_FDD_CMOS_1.92Msps_1.008MHz 4"
    exit 2
fi

STREAM_FILE="${PROFILE_DIR}/${PROFILE}.bin"
PROFILE_FILE="${PROFILE_DIR}/${PROFILE}.json"

if [ ! -f "$STREAM_FILE" ]; then
    echo "ERROR: stream file not found: $STREAM_FILE" >&2
    exit 1
fi
if [ ! -f "$PROFILE_FILE" ]; then
    echo "ERROR: profile file not found: $PROFILE_FILE" >&2
    exit 1
fi

# --- step 1: stream first, then profile --------------------------------------
# Both required; loading just profile leaves chip at default LVDS rate (15.36M)
# and dma_listen sees the DEFAULT-PROFILE sign-bit pattern. Stream provides
# the SSI framing microcode the profile then configures around.

echo "[1/5] loading stream  ${PROFILE}.bin"
cat "$STREAM_FILE" > "$IIO_PHY/stream_config"

echo "[1/5] loading profile ${PROFILE}.json"
cat "$PROFILE_FILE" > "$IIO_PHY/profile_config"

# --- step 2: tune LOs to W2 (5.6 / 5.8 GHz) ----------------------------------
# Profile defaults to 2.4/2.45 GHz which is out of W2 daughtercard band.
# Must happen AFTER profile load (profile resets LOs) and BEFORE cals
# (cals are LO-frequency-dependent).

echo "[2/5] tuning RX1 LO to $RX1_LO_HZ Hz (5.6 GHz, W2)"
iio_attr -c adrv9002-phy -o altvoltage0 frequency $RX1_LO_HZ > /dev/null

echo "[2/5] tuning TX1 LO to $TX1_LO_HZ Hz (5.8 GHz, W2)"
iio_attr -c adrv9002-phy -o altvoltage2 frequency $TX1_LO_HZ > /dev/null

# --- step 3: calibrations ----------------------------------------------------

echo "[3/5] running calibrations (safe 1T1R sequence)"
/home/root/oriinit-cli run-calibrations

# --- step 4: channelizer setup -----------------------------------------------
# Without these steps, dma_listen sees the channelizer's idle output
# (DEFAULT-PROFILE BINARY in dma_listen heuristic, sign-bits-only in raw bytes).

echo "[4/5] clearing channelizer stickies"
devmem $CH_STATUS 32 0x00000006

echo "[4/5] setting OUTPUT_SHIFT to $OUTPUT_SHIFT"
devmem $CH_OUTPUT_SHIFT 32 $(printf "0x%08X" $OUTPUT_SHIFT)

echo "[4/5] enabling channelizer"
devmem $CH_CONTROL 32 0x00000002

# Verify channelizer is producing frames
sleep 1
COUNT1=$(devmem $CH_FRAME_COUNT 32)
sleep 1
COUNT2=$(devmem $CH_FRAME_COUNT 32)
if [ "$COUNT1" = "$COUNT2" ]; then
    echo "WARNING: FRAME_COUNT not incrementing ($COUNT1)" >&2
else
    echo "      frame_count: $COUNT1 → $COUNT2 (live)"
fi

# --- step 5: summary report --------------------------------------------------

echo ""
echo "[5/5] bring-up complete — chip state:"
echo ""
/home/root/oriinit-cli status | grep -E "sample_rate|ensm|lo_hz" | sed 's/^/      /'
echo ""
echo "Verify real samples:  /home/root/dma_listen -c 10"
echo "Expect:               MULTI-BIT (real ADC samples?), distinct_high_I > 20"
echo "Inspect raw bytes:    iio_readdev -b 4096 -s 4096 axi-adrv9002-rx-lpc \\"
echo "                          2>/dev/null | od -A x -t x4 -v | head -20"
