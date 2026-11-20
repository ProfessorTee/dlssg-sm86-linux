#!/bin/sh
# Longshot with DLSS-G on RTX 20/30 (fixed 2x, DLSS FG forced); stdout/stderr -> logs/session.txt
HERE=$(CDPATH= cd -- "$(dirname -- "$(readlink -f -- "$0")")/.." && pwd)
mkdir -p "$HERE/logs"
cd "$HOME/Games/Longshot" || exit 1
export SDL_GAMECONTROLLER_IGNORE_DEVICES=0x1209/0xf00d SDL_GAMEPAD_IGNORE_DEVICES=0x1209/0xf00d
export RX_VK_FRAMEGEN_MULT="${RX_VK_FRAMEGEN_MULT:-4}" RX_VK_DLSS_FG="${RX_VK_DLSS_FG:-1}"
exec "$HERE/dlssg-sm86" ./bin/longshot "$@" > "$HERE/logs/session.txt" 2>&1
