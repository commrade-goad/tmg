#!/usr/bin/env bash

TMG="bin/tmg"
WD="$HOME/.local/share/tmg"

apply_color() {
    cd "$WD"
    local input="$1"
    local output="$2"

    $TMG \
        -i "$input" \
        -o "$output"
}

HYPRLANDCOLORPATH="$HOME/.config/hypr/colorscheme.conf"
ROFIPATH="$HOME/.config/rofi/sel.rasi"
DUNSTPATH="$HOME/.config/dunst/dunstrc"
EWWPATH="$HOME/.config/eww/eww.scss"
NVIMPATH="$HOME/.config/nvim/lua/colors/custom.lua"
ALAPATH="$HOME/.config/alacritty/sel.toml"

apply_color \
    "$WD/config/hypr-color.tmg" \
    "$HYPRLANDCOLORPATH"

apply_color \
    "$WD/config/rofi-color.tmg" \
    "$ROFIPATH"

apply_color \
    "$WD/config/dunst.tmg" \
    "$DUNSTPATH"

apply_color \
    "$WD/config/eww-color.tmg" \
    "$EWWPATH"

apply_color \
    "$WD/config/nvim-color.tmg" \
    "$NVIMPATH"

apply_color \
    "$WD/config/alacritty-color.tmg" \
    "$ALAPATH"

# RELOAD CONFIG
dunstctl reload
