#!/usr/bin/env bash
# Headless drive: Plugins → Apps → cabinet, walk through Music → Artists → Albums → Songs.
set -euo pipefail

DISP=:99
SIM_DIR=/home/brando/Projects/rockbox/build-ipodsim
SHOTS=/home/brando/Projects/ipod_theme/build/headless
mkdir -p "$SHOTS"

WIN=""

cap() {
    local name="$1"
    DISPLAY=$DISP ffmpeg -y -loglevel error \
        -f x11grab -video_size 1024x768 -i :99 \
        -frames:v 1 -update 1 "$SHOTS/$name.png"
    echo "  ✓ $name.png"
    ASDF_PYTHON_VERSION=3.14.4t python3 \
        /home/brando/Projects/ipod_theme/tools/render_index.py >/dev/null
}

key() {
    local k="$1"
    local n="${2:-1}"
    [[ -z "$WIN" ]] && WIN=$(DISPLAY=$DISP xdotool search --name "iPod Video" | head -1)
    for ((i = 0; i < n; i++)); do
        # Explicit keydown / hold / keyup so SDL sees a real press (the
        # default --key fires both edges back-to-back and Rockbox's button
        # state machine misses it).
        DISPLAY=$DISP xdotool keydown --window "$WIN" --clearmodifiers "$k"
        sleep 0.08
        DISPLAY=$DISP xdotool keyup --window "$WIN" --clearmodifiers "$k"
        sleep 0.18
    done
    sleep 0.4
}

# Restart sim cleanly
pkill -9 -f rockboxui 2>/dev/null || true
sleep 1
cd "$SIM_DIR"
DISPLAY=$DISP nohup ./rockboxui >/tmp/rockbox-headless.log 2>&1 &
disown
sleep 3

WIN=$(DISPLAY=$DISP xdotool search --name "iPod Video" | head -1)
echo "sim window: $WIN"

echo "[1] main menu"
cap p1-main

echo "[2] scroll to Plugins"
key Down 7
cap p2-plugins

echo "[3] enter Plugins menu (categories: Games / Applications / Demos)"
key Return
cap p3-plugins-categories

echo "[4] navigate Games -> Applications -> enter"
key Down 1
key Return
cap p4-apps-list

echo "[5] scroll to cabinet (after alarmclock, index 1)"
key Down 1
cap p5-cabinet-highlighted
key Return    # launch cabinet
sleep 1.0     # font load + initial draw
cap p6-cabinet-main

echo "[5] inside cabinet: Music"
# Music is index 0 — just Enter
key Return
cap p7-cabinet-music

echo "[6] Artists (index 0)"
key Return
sleep 1.0     # tagcache search may take a beat
cap p8-cabinet-artists

echo "[7] pick first artist"
key Return
sleep 1.0
cap p9-cabinet-albums

echo "[8] pick first album"
key Return
sleep 1.0
cap p10-cabinet-songs

echo "[9] pick first song → start playback → WPS"
key Return
sleep 2.0     # playlist build + audio start + WPS draw
cap p11-wps

echo "[10] cycle to NP info page 1 (big art)"
key Return
cap p12-wps-bigart

echo "[11] cycle to NP info page 2 (track info)"
key Return
cap p13-wps-trackinfo

echo "[12] cycle back to default"
key Return
cap p14-wps-default

echo "[13] wheel scroll → volume overlay"
key Down 2
cap p15-wps-volume

echo
echo "Done. $(ls -1 $SHOTS/p*.png | wc -l) screenshots in $SHOTS"
