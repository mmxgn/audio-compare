#!/usr/bin/env bash
# Asserts that one failing track branch no longer silences the whole mix.
# Builds a probe that counts buffers leaving the audiomixer, then runs it with a
# good file plus (a) another good file, (b) a missing file, (c) an undecodable
# file. Counts over two windows, before and after a flushing seek, because a
# dead branch used to wedge the mixer at both points.
# Run inside the dev shell:  nix develop -c ./verify.sh
set -eu
cd "$(dirname "$0")"
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT

cat >"$d/probe.c" <<'EOF'
// Replicates player_init + player_add and counts buffers leaving the mixer.
// Includes player.c so the static pipeline/mixer are reachable.
#include "player.c"
static guint      n, w1;
static GMainLoop *loop;
static Track     *ta, *tb;
static GstPadProbeReturn
count(GstPad *p, GstPadProbeInfo *i, gpointer u)
{
    n++;
    return GST_PAD_PROBE_OK;
}
static Track *
mk(const char *uri)
{
    Track *t    = g_new0(Track, 1);
    t->uri      = g_strdup(uri);
    t->name     = g_path_get_basename(uri);
    t->bus      = -1;
    t->duration = -1;
    return t;
}
// Same bookkeeping the app's handler does: drop the handles player.c is about
// to tear down.
static void
track_failed(GstElement *branch, const char *msg)
{
    if (tb->branch != branch)
        return;
    tb->failed = TRUE;
    tb->branch = tb->vol = tb->amp = NULL;
    tb->vol_cs = tb->amp_cs = NULL;
    tb->mixpad              = NULL;
}
static gboolean
phase(gpointer u)
{
    static int step;
    if (step++ == 0) {
        w1 = n;
        n  = 0;
        player_seek(500 * GST_MSECOND);
        return G_SOURCE_CONTINUE;
    }
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}
int
main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);
    player_set_error_handler(track_failed);
    player_init();
    ta = mk(argv[1]);
    tb = mk(argv[2]);
    player_add(ta);
    player_add(tb);
    GstPad *src = gst_element_get_static_pad(mixer, "src");
    gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, count, NULL, NULL);
    gst_object_unref(src);
    player_set_audible(ta, TRUE, 0);
    player_set_audible(tb, TRUE, 0);
    player_play();
    g_timeout_add(1500, phase, NULL);
    g_main_loop_run(loop);
    g_print("before_seek=%u after_seek=%u position=%" G_GINT64_FORMAT "\n", w1, n,
            player_position());
    return 0;
}
EOF

gcc -o "$d/probe" "$d/probe.c" -I src $(pkg-config --cflags --libs \
    gstreamer-1.0 gstreamer-controller-1.0 glib-2.0 gobject-2.0)

gst-launch-1.0 -q audiotestsrc num-buffers=400 ! audioconvert ! wavenc \
    ! filesink location="$d/good.wav"
printf 'not audio at all' >"$d/bad.wav"

fail=0
check() { # label uri-of-second-track
    out=$(GST_DEBUG=0 "$d/probe" "file://$d/good.wav" "$2" 2>/dev/null | tail -1)
    echo "  $1: $out"
    eval "${out// /;}" # before_seek=.. after_seek=.. position=..
    # ~100 buffers/s out of the mixer over each 1.5 s window
    [ "$before_seek" -gt 100 ] || { echo "    FAIL: $before_seek buffers before seek"; fail=1; }
    [ "$after_seek" -gt 100 ] || { echo "    FAIL: $after_seek buffers after seek"; fail=1; }
    [ "$position" -gt 0 ] || { echo "    FAIL: player_position() = $position"; fail=1; }
}

check "two good files " "file://$d/good.wav"
check "missing file   " "file://$d/does-not-exist.wav"
check "undecodable    " "file://$d/bad.wav"

[ $fail -eq 0 ] && echo "PASS" || { echo "FAILED"; exit 1; }
