#!/bin/sh
# Build and run the automated checks for the UI/state fixes (bugs 1, 6, 7, 8).
# The remaining fixes (2, 3, 4, 5) are GUI behaviour -- see the manual steps in
# the branch report.
set -eu
cd "$(dirname "$0")"

run() { if [ -n "${IN_NIX_SHELL:-}" ]; then "$@"; else nix develop -c "$@"; fi; }

[ -d build ] || run meson setup build
run ninja -C build
for bug in 1 6 7 8; do
    printf 'bug %s: ' "$bug"
    run ./build/check "$bug"
done

# A structurally valid WAV with zero audio frames, for the manual bug 7 step:
# open it in the app and click the far right of its pane.
printf 'RIFF$\0\0\0WAVEfmt \20\0\0\0\1\0\1\0\104\254\0\0\210X\1\0\2\0\20\0data\0\0\0\0' >build/empty.wav
echo "wrote build/empty.wav (header only, 0 frames) for the manual bug 7 step"
