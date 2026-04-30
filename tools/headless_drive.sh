#!/usr/bin/env bash
# Drive the rockboxui sim on Xvfb :99 and capture labeled screenshots.
# Usage: ./headless_drive.sh
set -euo pipefail

DISP=:99
SIM_DIR=/home/brando/Projects/rockbox/build-ipodsim
SHOTS=/home/brando/Projects/ipod_theme/build/headless
mkdir -p "$SHOTS"

cap() {
    local name="$1"
    DISPLAY=$DISP ffmpeg -y -loglevel error \
        -f x11grab -video_size 1024x768 -i :99 \
        -frames:v 1 -update 1 "$SHOTS/$name.png"
    echo "  saved $name.png"
}

WIN=""
find_window() {
    WIN=$(DISPLAY=$DISP xdotool search --name "iPod Video" | head -1)
}

key() {
    local k="$1"
    local n="${2:-1}"
    [[ -z "$WIN" ]] && find_window
    for ((i = 0; i < n; i++)); do
        DISPLAY=$DISP xdotool key --window "$WIN" --clearmodifiers "$k"
        sleep 0.2
    done
    sleep 0.4   # let SDL process and re-render
}

ensure_sim() {
    if ! pgrep -f './rockboxui' >/dev/null; then
        cd "$SIM_DIR"
        DISPLAY=$DISP nohup ./rockboxui >/tmp/rockbox-headless.log 2>&1 &
        disown
        sleep 3
    fi
}

ensure_sim

echo "[1] capture main menu"
cap 01-main

echo "[2] scroll down through main menu (find Plugins — index 8)"
# Sim opens at "Files" (index 1). Plugins is index 8.
key Down 7
sleep 0.2
cap 02-plugins-highlighted

echo "[3] enter Plugins"
key Return
sleep 0.4
cap 03-plugins-menu

echo "[4] enter Apps (usually first)"
# Apps may not be first; capture lets us see.
key Return
sleep 0.4
cap 04-apps-menu

echo "[5] scroll to find cabinet — alphabetical order"
# alarmclock=0 ... cabinet should be early (around index 1-2 after alarmclock+amaze+...)
# Just try paging down 1-2 times if not visible
key Down 2
sleep 0.2
cap 05-near-cabinet

echo
echo "Screenshots saved to $SHOTS"
ls -la "$SHOTS"
