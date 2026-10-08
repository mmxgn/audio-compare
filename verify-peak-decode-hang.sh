#!/usr/bin/env bash
# Verifies that extract_peaks() fails fast on undecodable input instead of
# hanging the main thread, and still decodes valid files correctly.
# Run from the repo root:  nix develop -c ./verify.sh
set -u

cd "$(dirname "$0")"
W=build/verify
IN=$W/in
rm -rf $W
mkdir -p $IN

echo "== build =="
meson setup build >/dev/null || true
ninja -C build >/dev/null || exit 1

# Harness: track_new() without the GUI, so a hang is visible to `timeout`.
cat >$W/harness.c <<'EOF'
#include "track.h"
#include <math.h>
#include <stdio.h>
int
main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    Track *t = track_new(argv[1]);
    printf("RESULT peaks=%u duration_s=%.2f\n", t->peaks->len,
           t->duration < 0 ? -1.0 : (double)t->duration / GST_SECOND);
    return 0;
}
EOF
CFLAGS=$(pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gio-2.0 libebur128)
LIBS=$(pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 gio-2.0 libebur128)
gcc -Isrc -o $W/harness $W/harness.c src/track.c $CFLAGS $LIBS -lm || exit 1

echo "== inputs =="
mkdir -p $IN/a_directory
printf 'this is not a wav file\n' >$IN/text.wav
gst-launch-1.0 -q videotestsrc num-buffers=1 ! pngenc ! filesink location=$IN/image.png
gst-launch-1.0 -q videotestsrc num-buffers=30 ! x264enc ! matroskamux ! \
    filesink location=$IN/videoonly.mkv
# 480 * 1000 samples @ 48 kHz = exactly 10 s
gst-launch-1.0 -q audiotestsrc num-buffers=480 samplesperbuffer=1000 ! \
    audio/x-raw,rate=48000,channels=1 ! wavenc ! filesink location=$IN/good.wav
gst-launch-1.0 -q audiotestsrc num-buffers=480 samplesperbuffer=1000 ! \
    audio/x-raw,rate=48000,channels=1 ! audioconvert ! lamemp3enc ! \
    filesink location=$IN/good.mp3
head -c $(( $(stat -c%s $IN/good.mp3) / 3 )) $IN/good.mp3 >$IN/truncated.mp3

fail=0

# run <label> <uri> <expect: dead|alive>
run()
{
    out=$(timeout 20 $W/harness "$2" 2>&1)
    rc=$?
    if [ $rc -eq 124 ]; then
        echo "FAIL $1: HUNG (killed after 20s)"
        fail=1
        return
    fi
    res=$(printf '%s' "$out" | grep -o 'RESULT.*')
    warn=$(printf '%s' "$out" | grep -c 'WARNING')
    if [ "$3" = dead ]; then
        if [ "$res" = "RESULT peaks=0 duration_s=-1.00" ] && [ "$warn" -ge 1 ]; then
            echo "PASS $1: rejected ($res, warned)"
        else
            echo "FAIL $1: expected empty track + warning, got [$res] warnings=$warn"
            fail=1
        fi
    else
        echo "PASS $1: $res"
    fi
}

echo "== bad inputs (must not hang) =="
run directory "file://$PWD/$IN/a_directory" dead
run png "file://$PWD/$IN/image.png" dead
run "text-as-wav" "file://$PWD/$IN/text.wav" dead
run nonexistent "file://$PWD/$IN/nope.wav" dead
run "video-only-mkv" "file://$PWD/$IN/videoonly.mkv" dead

echo "== good inputs (must still decode) =="
run wav "file://$PWD/$IN/good.wav" alive
run mp3 "file://$PWD/$IN/good.mp3" alive
run "truncated-mp3" "file://$PWD/$IN/truncated.mp3" alive

# 10 s @ 48 kHz / 512 = 938 peaks
timeout 20 $W/harness "file://$PWD/$IN/good.wav" 2>/dev/null |
    grep -q 'peaks=938 duration_s=10.01' ||
    { echo "FAIL wav: wrong peak count/duration (expected 938 / 10.01 s)"; fail=1; }

[ $fail -eq 0 ] && echo "ALL OK" || echo "FAILURES"
exit $fail
