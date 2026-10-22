#!/usr/bin/env sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
deps="$root/deps"
mkdir -p "$deps"

clone() {
    dir="$1"; tag="$2"; url="$3"
    if [ ! -d "$deps/$dir/.git" ]; then
        git clone --depth 1 --branch "$tag" "$url" "$deps/$dir"
    fi
}

clone libuv v1.53.0 https://github.com/libuv/libuv.git
clone llhttp release/v9.4.3 https://github.com/nodejs/llhttp.git
