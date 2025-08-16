#!/usr/bin/env bash

TMG_WALL="bin/tmg-wall"
WD="$HOME/.local/share/tmg"
WALLPAPER=$1

$TMG_WALL "$WALLPAPER" "$WD/lib/color-def.lua"
"$WD/apply.sh"
