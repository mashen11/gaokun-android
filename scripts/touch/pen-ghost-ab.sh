#!/usr/bin/env bash
# pen-ghost-ab.sh -- one-command A/B harness for the gaokun3 "one finger acts
# like two" ghost-touch bug.
#
# The bug: the pen path may birth a pen track on a frame where the amplitude
# path found 0 contacts, while a finger track is still alive in its
# track_lost_frames grace period.  The same finger blob is then reported twice
# -- slot A as MT_TOOL_FINGER, slot B as MT_TOOL_PEN -- and Android reads the
# second pointer as a pinch.  Symptoms on the device: one-finger zoom in an
# image app, cancelled recents swipe-up, and a vertical media-gesture swipe
# turning into a video zoom.  Root cause + fix: patch 0082.  See
#   docs/stylus.md  ("幽灵触点：一指变两指").
#
# This script does the three things that must happen together, host-side:
#   1. snapshot + clear the driver's per-stage counters,
#   2. capture /dev/input/eventN for N seconds while you perform the gestures,
#   3. read the counters back, pull the capture, and run the analysers.
#
# Usage (from the repo root):
#     bash scripts/touch/pen-ghost-ab.sh 60 "nbr8=150"
#     bash scripts/touch/pen-ghost-ab.sh 60 "nbr8=367"     # gate re-armed, for contrast
#
# While it runs, do -- WITH THE PEN WELL AWAY FROM THE PANEL -- several rounds of:
#     * one-finger pinch in an image app (does a single finger zoom?)
#     * one-finger swipe up from the bottom edge (does Recents open?)
#     * a long vertical swipe in a full-screen video (does it zoom instead?)
# The pen-free condition is the whole point: if a pen track appears with no pen
# near the panel, it is unambiguously a ghost.
#
# Env overrides: DEVHOST (adb serial; default = the single attached device),
#                EVDEV, ALGO_GLOB, GHOST_TMP.

set -u

# Device selection.  Set DEVHOST=192.168.31.177:5555 (or any adb serial) to pin
# one device; otherwise plain `adb` is used, which is right when exactly one
# device is attached.  No address is baked in.
DEV=${DEVHOST:-}

EVDEV=${EVDEV:-/dev/input/event10}
ALGO_GLOB=${ALGO_GLOB:-/sys/bus/spi/devices/*/algo}

SECS=${1:-60}
LABEL=${2:-run}

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TMP=${GHOST_TMP:-/tmp/ghost-ab}
mkdir -p "$TMP"
STAMP=$(date +%Y%m%d-%H%M%S)
CAP="$TMP/ghost-ab-$LABEL-$STAMP.txt"
RSTATS="$TMP/ghost-ab-$LABEL-$STAMP.stats"

say() { printf '%s\n' "$*"; }
ADB() { if [ -n "$DEV" ]; then adb -s "$DEV" "$@"; else adb "$@"; fi; }
sh_dev() { ADB shell "su -c '$1'"; }

say "=============================================================="
say "gaokun3 ghost-touch A/B -- label=$LABEL, ${SECS}s"
say "  device : ${DEV:-(the single attached device)}"
say "  evdev  : $EVDEV"
say "  capture: $CAP"
say "=============================================================="

# ---- sanity: device + algo group -------------------------------------------
if ! ADB get-state >/dev/null 2>&1; then
    say "!! no device.  Try: adb connect <serial>, or set DEVHOST=<serial>"
    exit 2
fi

ALGO=$(sh_dev "for d in $ALGO_GLOB; do [ -e \"\$d/pen_enabled\" ] && { echo \"\$d\"; break; }; done" | tr -d '\r')
if [ -z "$ALGO" ]; then
    say "!! no algo group found -- panel has not probed.  dmesg | grep -i himax"
    exit 2
fi
say "algo group: $ALGO"
say "knobs in effect:"
sh_dev "for k in pen_enabled pen_min_nbr8 pen_tap_level pen_threshold pen_confirm_frames pen_hold_frames pen_latch_travel pen_geom_enable pen_geom_pow; do printf '  %-20s %s\n' \$k \$(cat $ALGO/\$k 2>/dev/null); done"

# ---- 1. clear counters ------------------------------------------------------
sh_dev "echo 0 > $ALGO/stats" >/dev/null 2>&1
say ""
say "[1/3] counters cleared."

# ---- 2. capture (host-side, so adb shell teardown cannot SIGHUP it) --------
say "[2/3] capturing ${SECS}s -- DO THE GESTURES NOW (pen away from panel)."
ADB shell "su -c 'timeout $SECS getevent -lt $EVDEV > /data/local/tmp/ghost-ab.txt 2>/dev/null; echo DONE > /data/local/tmp/ghost-ab.done'" >/dev/null 2>&1 &
CAP_PID=$!

# progress ticks so the operator knows it is alive
for ((i = SECS; i > 0; i -= 10)); do
    sleep 10
    printf '      ... %ds left\n' "$i" 2>/dev/null || true
done
wait "$CAP_PID" 2>/dev/null

# ---- 3. read counters + analyse -------------------------------------------
say "[3/3] done.  Reading counters."
sh_dev "cat $ALGO/stats" | tr -d '\r' > "$RSTATS"

ADB shell "su -c 'cat /data/local/tmp/ghost-ab.txt'" > "$CAP" 2>/dev/null

say ""
say "---- driver stage counters (this run) ----"
grep -E '^(frames|contacts|tracks_new|max_contacts|slots_born_pen|slots_born_finger|pen_frame_cand|pen_ghost_runs|pen_latch_move|pen_latch_amp|pen_latch_hold|pen_frame_emit|pen_dec_wait|pen_dec_amp_short|pen_dec_spatial|pen_dec_finger)' "$RSTATS" \
    | awk '{printf "  %-20s %s\n", $1, $2}'

say ""
say "---- evdev analysis ----"
python3 "$ROOT/scripts/touch/pen-multitouch-analyze.py" "$CAP" 2>/dev/null \
    | sed -n '1,20p'

say ""
say "=============================================================="
say "HOW TO READ IT"
say "  slots_born_pen  : pen-shaped tracks born this run."
say "                    pen-free finger work => should be ~0."
say "  pen_dec_finger  : latches refused because a non-pen (finger) track"
say "                    was still live (0082's new counter).  With 0082 in"
say "                    the driver this is the ghost, being refused."
say "  pen_dec_spatial : latches the nbr8 gate refused.  ~0 at nbr8=150 (the"
say "                    patched default), clearly non-zero at nbr8=367."
say "  evdev 'finger/pen' multiset (NOT finger/finger) + a non-zero"
say "                    'two points < 60 px apart' count = the ghost."
say "  A pass = slots_born_pen ~0 AND '< 60 px' frames ~0, while the pen"
say "           still writes normally (check that separately, with the pen)."
say "=============================================================="
