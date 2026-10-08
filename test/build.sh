#!/usr/bin/env bash
# Build the measurement harness. $1 = extra cflags (e.g. -DOLD), $2 = output,
# $3 = player.c to link against (default src/player.c).
set -e
cd "$(dirname "$0")/.."
flags=$(pkg-config --cflags --libs gstreamer-1.0 gstreamer-controller-1.0 glib-2.0 gobject-2.0)
# shellcheck disable=SC2086
gcc $1 -g -O0 -Isrc test/harness.c "${3:-src/player.c}" -o "$2" $flags -lm
