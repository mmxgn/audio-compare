#!/usr/bin/env bash
# Demonstrates the "seek immediately after a state restore is lost" fix.
#
# Builds a GTK-less harness against src/player.c twice: once as-is, once with
# the player_wait_ready() calls stripped out (the pre-fix code). Without the
# wait the branch's uridecodebin has not re-exposed its pad yet, the flushing
# seek aborts on the unlinked pad, the branch pads stay flushing and playback
# dies with the position pinned at 0.
#
# Run from the repo root inside `nix develop`:  ./verify.sh
set -euo pipefail
cd "$(dirname "$0")"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT

echo "== generating 35s test tones =="
for f in a:440 b:660; do
  gst-launch-1.0 -q audiotestsrc num-buffers=1500 freq="${f#*:}" \
    ! audioconvert ! wavenc ! filesink location="$W/${f%%:*}.wav"
done

cat >"$W/harness.c" <<'EOF'
// Replicates main.c's use of the player API: two tracks, play to 4s, then
// either close one (player_remove) or rebuild the engine (reset_engine).
#include "player.h"
#include <stdio.h>
#include <string.h>

static Track t0, t1;

static double sec(gint64 ns) { return ns < 0 ? -1.0 : ns / 1e9; }

static void add_both(void)
{
    player_add(&t0);
    player_add(&t1);
    player_set_audible(&t0, TRUE, 0);
    player_set_audible(&t1, FALSE, 0);
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    const char *mode = argv[1];
    t0.uri = argv[2];
    t1.uri = argv[3];

    player_init();
    add_both();
    player_play();
    // "early": close a track while the pipeline is still prerolling, so the
    // state query races the PLAYING transition.
    g_usleep(strcmp(mode, "early") ? 4 * G_USEC_PER_SEC : 0);

    gint64 before = player_position();
    printf("  pos before %s = %.3fs\n", mode, sec(before));

    if (!strcmp(mode, "remove") || !strcmp(mode, "early")) {
        player_remove(&t0);
    } else { // reset_engine() sequence from main.c
        gint64 pos = player_position();
        if (pos < 0) pos = 0;
        player_shutdown();
        player_init();
        add_both();
        if (!strcmp(mode, "reset-wait")) player_wait_ready();
        if (pos > 0) player_seek(pos);
        player_play();
    }

    gboolean ok = FALSE;
    for (int i = 1; i <= 3; i++) {
        g_usleep(G_USEC_PER_SEC);
        gint64 p = player_position();
        printf("    t+%ds: pos=%.3fs\n", i, sec(p));
        if (p > before) ok = TRUE;
    }
    printf("  -> %s\n", ok ? "position advances" : "POSITION PINNED");
    player_shutdown();
    return ok ? 0 : 1;
}
EOF

# Pre-fix copy of player.c: drop the player_wait_ready() call and put the state
# query back on a zero timeout.
mkdir -p "$W/pre"
sed -e '/player_wait_ready();/d' \
    -e 's/&state, NULL, PREROLL_WAIT/\&state, NULL, 0/' src/player.c >"$W/pre/player.c"
cp src/player.h src/track.h "$W/pre/"

CFLAGS=$(pkg-config --cflags gstreamer-1.0 gstreamer-controller-1.0)
LIBS=$(pkg-config --libs gstreamer-1.0 gstreamer-controller-1.0)
# shellcheck disable=SC2086
cc -I src        -o "$W/post" "$W/harness.c" src/player.c       $CFLAGS $LIBS
# shellcheck disable=SC2086
cc -I "$W/pre"   -o "$W/pre.bin" "$W/harness.c" "$W/pre/player.c" $CFLAGS $LIBS

A="file://$W/a.wav" B="file://$W/b.wav"
rc=0
run() { # <label> <binary> <mode> <expect-pass 0|1>
  echo "== $1 =="
  if "$2" "$3" "$A" "$B"; then got=1; else got=0; fi
  [ "$got" = "$4" ] || { echo "  !! UNEXPECTED"; rc=1; }
}

run "BEFORE: close a track"  "$W/pre.bin" remove       0
run "AFTER:  close a track"  "$W/post"    remove       1
run "BEFORE: close while prerolling" "$W/pre.bin" early 0
run "AFTER:  close while prerolling" "$W/post"    early 1
run "BEFORE: reset engine"   "$W/post"    reset-nowait 0
run "AFTER:  reset engine"   "$W/post"    reset-wait   1

[ $rc = 0 ] && echo "ALL EXPECTATIONS MET" || echo "SOME EXPECTATIONS NOT MET"
exit $rc
