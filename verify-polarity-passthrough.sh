#!/usr/bin/env bash
# Null test for the polarity-invert feature. Builds two player branches from the
# real src/player.c (included, not copied, so this tests the shipped code), feeds
# both the same file, inverts one, and measures the peak of the summed mixer
# output. Correct inversion cancels; a no-op sums to +6 dB.
# Run inside `nix develop`.
set -euo pipefail
cd "$(dirname "$0")"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export GST_PLUGIN_FEATURE_RANK=fakeaudiosink:MAX # keep autoaudiosink off the speakers

gst-launch-1.0 -q audiotestsrc wave=pink-noise num-buffers=250 \
    ! audioconvert ! audio/x-raw,format=F32LE,channels=2,rate=48000 \
    ! wavenc ! filesink location="$T/in.wav"

cat > "$T/nulltest.c" << 'EOF'
#include "player.c" // for the real branch wiring and the static `mixer`

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLOATS_PER_SEC (48000 * 2)

static Track  trk[2];
static double peak, step;
static gint64 nfloat, skip;

// Peak of the summed mixer output, ignoring everything before `skip`.
static GstPadProbeReturn
on_mix(GstPad *pad, GstPadProbeInfo *info, gpointer u)
{
    GstMapInfo m;
    gst_buffer_map(GST_PAD_PROBE_INFO_BUFFER(info), &m, GST_MAP_READ);
    const float *f = (const float *)m.data;
    for (gsize i = 0; i < m.size / sizeof(float); i++, nfloat++)
        if (nfloat > skip && fabs(f[i]) > peak)
            peak = fabs(f[i]);
    gst_buffer_unmap(GST_PAD_PROBE_INFO_BUFFER(info), &m);
    return GST_PAD_PROBE_OK;
}

// Flip polarity, recording the discontinuity it writes into the control source:
// the old curve against the new one, both read 100 us ahead so the sample point
// is past the new ramp's first control point.
static gboolean
toggle(gpointer u)
{
    Track       *t  = u;
    GstClockTime at = (GstClockTime)player_running_time() + 100 * GST_USECOND;
    double       a = 0, b = 0;
    gst_control_source_get_value(t->amp_cs, at, &a);
    t->inverted = !t->inverted;
    player_set_inverted(t, t->inverted);
    gst_control_source_get_value(t->amp_cs, at, &b);
    if (fabs(b - a) > step)
        step = fabs(b - a);
    return G_SOURCE_REMOVE;
}

static gboolean
quit(gpointer loop)
{
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int
main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    int mode = atoi(argv[2]);     // 0 both normal, 1 inverted from the start,
    int n    = mode == 4 ? 1 : 2; // 2 toggled mid-play, 3 triple-toggled, 4 one track
    skip     = (mode == 2 || mode == 3 ? 3 : 1) * FLOATS_PER_SEC;

    player_init();
    trk[1].inverted = mode == 1;
    for (int i = 0; i < n; i++) {
        trk[i].uri = argv[1];
        player_add(&trk[i]);
        player_set_audible(&trk[i], TRUE, 1);
    }

    GstPad *mp = gst_element_get_static_pad(mixer, "src");
    gst_pad_add_probe(mp, GST_PAD_PROBE_TYPE_BUFFER, on_mix, NULL, NULL);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    player_play();
    if (mode == 2)
        g_timeout_add(1500, toggle, &trk[1]);
    if (mode == 3)
        for (int ms = 1500; ms <= 1520; ms += 10) // three presses inside one 30 ms ramp
            g_timeout_add(ms, toggle, &trk[1]);
    g_timeout_add(4500, quit, loop);
    g_main_loop_run(loop);

    GstCaps *c = gst_pad_get_current_caps(mp);
    char    *s = gst_caps_to_string(c);
    if (!strstr(s, "F32LE")) {
        fprintf(stderr, "harness: expected F32LE on the mixer, got %s\n", s);
        return 2;
    }
    printf("%.1f %.4f\n", peak > 0 ? 20 * log10(peak) : -200.0, step);
    return 0;
}
EOF

cc -o "$T/nulltest" "$T/nulltest.c" -Isrc \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-controller-1.0) -lm

fail=0
check() { # label mode <=|>= threshold-dBFS
    read -r db st < <("$T/nulltest" "file://$T/in.wav" "$2")
    printf '%-34s %8s dBFS  max control step %s\n' "$1" "$db" "$st"
    awk -v d="$db" -v t="$4" -v o="$3" \
        'BEGIN{exit !(o=="<=" ? d<=t : d>=t)}' || { echo "  FAIL: want $3 $4 dBFS"; fail=1; }
    awk -v s="$st" 'BEGIN{exit !(s<=0.05)}' || { echo "  FAIL: control source jumped $st"; fail=1; }
}

check "one track (reference)" 4 ">=" -200
check "two tracks, neither inverted" 0 ">=" -200
check "one inverted from the start" 1 "<=" -80
check "inverted mid-playback" 2 "<=" -80
check "three presses in one ramp" 3 "<=" -80
exit $fail
