#!/usr/bin/env bash
# Asserts that an audible-set switch lands cleanly in every pipeline state.
# Run it inside the dev shell:  nix develop -c ./verify.sh
#
# Builds test/harness.c against src/player.c, feeds it two sine files of known
# amplitude and probes each branch's output, so the gain the control source
# actually applied is read back per buffer. Set PLAYER=<file> to measure a
# different player.c (e.g. the pre-fix one) and CFLAGS=-DOLD to use the old
# running-time call site.
set -u
cd "$(dirname "$0")"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

gen() { # gen <file> <freq> <amplitude>
    gst-launch-1.0 -q audiotestsrc wave=sine freq="$2" volume="$3" num-buffers=469 \
        ! audioconvert ! audio/x-raw,format=F32LE,rate=48000,channels=2 \
        ! wavenc ! filesink location="$1"
}
gen "$work/a.wav" 440 0.05   # 10 s, amplitude 0.05
gen "$work/b.wav" 1000 0.025 # 10 s, amplitude 0.025

test/build.sh "${CFLAGS:-}" "$work/harness" "${PLAYER:-src/player.c}" 2>/dev/null

rc=0
for s in baseline pause seek eos premute count soak; do
    "$work/harness" "$s" "$work/a.wav" "$work/b.wav" || rc=1
done
echo
[ $rc -eq 0 ] && echo "all scenarios PASS" || echo "SOME SCENARIOS FAILED"
exit $rc
