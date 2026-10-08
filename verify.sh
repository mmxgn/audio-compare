#!/usr/bin/env bash
# Verifies that track.c measures BS.1770 integrated loudness and true peak on the
# file's real channel layout, not a mono downmix. Compares against ffmpeg's
# independent ebur128 implementation.
#
#   ./verify.sh                       # measures ./src/track.c
#   TRACK_C=/path/to/old/track.c ./verify.sh   # measures some other revision
set -u

TOL_LUFS=0.3 # LU
TOL_TP=0.3   # dB

root=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

for c in python3 ffmpeg gcc pkg-config; do
    command -v "$c" >/dev/null || { echo "missing: $c (run inside 'nix develop')"; exit 1; }
done

# --- test signals -----------------------------------------------------------
python3 - "$work" <<'PY'
import math, struct, sys, wave
d, sr, n = sys.argv[1], 48000, 48000 * 10

def s(f, a=0.5, ph=0.0):
    return [a * math.sin(2 * math.pi * f * i / sr + ph) for i in range(n)]

def add(x, y):
    return [a + b for a, b in zip(x, y)]

def write(name, chans, frames=None):
    frames = frames or n
    # 10 ms raised-cosine fades: a hard start at full amplitude rings the true-peak
    # oversampling filter and would make the test signal, not the code, the variable.
    r = int(sr * 0.01)
    env = lambda i: 0.5 - 0.5 * math.cos(math.pi * min(i, frames - 1 - i, r) / r)
    w = wave.open("%s/%s.wav" % (d, name), "wb")
    w.setnchannels(len(chans)); w.setsampwidth(2); w.setframerate(sr)
    out = bytearray()
    for i in range(frames):
        g = env(i)
        for c in chans:
            out += struct.pack("<h", max(-32768, min(32767, int(c[i] * g * 32767))))
    w.writeframes(bytes(out)); w.close()

L = s(997)
write("centre",   [L, L])                                      # fully correlated
write("typical",  [add(L, s(1493, .2)), add(L, s(2311, .2))])   # centre + width
write("wide",     [L, s(997, ph=math.pi / 2)])                 # uncorrelated, equal power
write("hardpan",  [s(997, .99), [0.0] * n])                    # all in L, peak near 0 dBFS
write("outphase", [L, [-v for v in L]])                        # L = -R
write("mono",     [L])
write("short",    [L, L], frames=int(sr * 0.2))                # < 400 ms, no gating block
PY

# --- probe: the app's own track.c ------------------------------------------
cat >"$work/probe.c" <<'EOF'
#include "track.h"
#include <stdio.h>
int
main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    for (int i = 1; i < argc; i++) {
        char  *uri = gst_filename_to_uri(argv[i], NULL);
        Track *t   = track_new(uri);
        printf("%.2f %.2f\n", t->lufs, t->dbtp);
        track_free(t);
        g_free(uri);
    }
    return 0;
}
EOF
flags=$(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gio-2.0 libebur128)
gcc -O1 -I"$root/src" -o "$work/probe" "$work/probe.c" "${TRACK_C:-$root/src/track.c}" $flags -lm || exit 1

# ffmpeg ebur128: integrated loudness and true peak, both on the real layout
reference() {
    ffmpeg -nostats -hide_banner -i "$1" -af ebur128=peak=true -f null - 2>&1 |
        awk '/^ +I: /{i=$2} /^ +Peak: /{p=$2} END{print i, p}'
}

fail=0
printf '%-9s %9s %9s %9s %9s %s\n' signal app_LUFS ref_LUFS app_dBTP ref_dBTP result
for f in centre typical wide hardpan outphase mono short; do
    read -r al ad < <("$work/probe" "$work/$f.wav" 2>/dev/null)
    read -r rl rd < <(reference "$work/$f.wav")
    verdict=$(python3 -c '
import sys
al, ad, rl, rd, tl, td, name = sys.argv[1:]
f = lambda x: float(x)
bad = []
if name == "short":
    # no gating block completes under 400 ms: -inf LUFS is correct, dBTP must be valid
    if f(al) != float("-inf"): bad.append("LUFS not -inf")
    if abs(f(ad) - f(rd)) > f(td): bad.append("dBTP")
else:
    if abs(f(al) - f(rl)) > f(tl): bad.append("LUFS")
    if abs(f(ad) - f(rd)) > f(td): bad.append("dBTP")
print("FAIL: " + ",".join(bad) if bad else "ok")' "$al" "$ad" "$rl" "$rd" "$TOL_LUFS" "$TOL_TP" "$f")
    printf '%-9s %9s %9s %9s %9s %s\n' "$f" "$al" "$rl" "$ad" "$rd" "$verdict"
    [ "$verdict" = ok ] || fail=1
done

# centre and wide have identical true loudness; the app must not invent a difference
read -r cl _ < <("$work/probe" "$work/centre.wav" 2>/dev/null)
read -r wl _ < <("$work/probe" "$work/wide.wav" 2>/dev/null)
same=$(python3 -c "print('ok' if abs($cl - $wl) <= $TOL_LUFS else 'FAIL')")
printf '\ncentre vs wide: %s vs %s LUFS -> %s (no phantom width difference)\n' "$cl" "$wl" "$same"
[ "$same" = ok ] || fail=1

[ "$fail" = 0 ] && echo "PASS (tolerance ${TOL_LUFS} LU / ${TOL_TP} dB)" || echo "FAILED"
exit $fail
