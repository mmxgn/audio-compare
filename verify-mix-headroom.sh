#!/usr/bin/env bash
# Asserts the mix bus never reaches full scale at the audio sink.
# Builds a throwaway harness replicating player_init() + N player_add() branches
# from src/player.c, feeds it N copies of one file and probes the pad feeding
# autoaudiosink. Run inside `nix develop`.
set -euo pipefail

HEADROOM=0.25 # keep in sync with MIX_HEADROOM in src/player.c
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT

for fmt in S16LE F32LE; do
    gst-launch-1.0 -q audiotestsrc wave=sine freq=440 volume=0.4 num-buffers=50 \
        ! audioconvert ! audioresample ! "audio/x-raw,format=$fmt,channels=2,rate=48000" \
        ! wavenc ! filesink location="$D/$fmt.wav"
done

cat >"$D/h.c" <<'EOF'
#include <gst/controller/gstdirectcontrolbinding.h>
#include <gst/controller/gstinterpolationcontrolsource.h>
#include <gst/gst.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static double peak;
static guint64 total, over;
static GMainLoop *loop;

static GstPadProbeReturn on_buf(GstPad *pad, GstPadProbeInfo *info, gpointer u) {
  GstCaps *c = gst_pad_get_current_caps(pad);
  if (!c) return GST_PAD_PROBE_OK;
  const char *f = gst_structure_get_string(gst_caps_get_structure(c, 0), "format");
  GstMapInfo m;
  if (gst_buffer_map(GST_PAD_PROBE_INFO_BUFFER(info), &m, GST_MAP_READ)) {
    gsize n = m.size / (!g_strcmp0(f, "S16LE") ? 2 : 4);
    for (gsize i = 0; i < n; i++) {
      double a = !g_strcmp0(f, "S16LE") ? fabs(((gint16 *)m.data)[i]) / 32768.0
               : !g_strcmp0(f, "S32LE") ? fabs(((gint32 *)m.data)[i]) / 2147483648.0
                                        : fabs(((float *)m.data)[i]);
      if (a > peak) peak = a;
      if (a >= 1.0) over++;
    }
    total += n;
    gst_buffer_unmap(GST_PAD_PROBE_INFO_BUFFER(info), &m);
  }
  gst_caps_unref(c);
  return GST_PAD_PROBE_OK;
}

static void on_pad_added(GstElement *d, GstPad *p, gpointer u) {
  GstPad *s = gst_element_get_static_pad(u, "sink");
  if (!gst_pad_is_linked(s)) gst_pad_link(p, s);
  gst_object_unref(s);
}

static gboolean on_bus(GstBus *b, GstMessage *m, gpointer u) {
  if (GST_MESSAGE_TYPE(m) & (GST_MESSAGE_EOS | GST_MESSAGE_ERROR)) g_main_loop_quit(loop);
  return TRUE;
}

int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  int n = atoi(argv[1]);
  GstElement *pipeline = gst_pipeline_new("player");
  GstElement *mixer = gst_element_factory_make("audiomixer", "mix");
  GstElement *master = gst_element_factory_make("volume", "master");
  GstElement *conv = gst_element_factory_make("audioconvert", NULL);
  GstElement *sink = gst_element_factory_make("autoaudiosink", NULL);
  g_object_set(master, "volume", atof(argv[3]), NULL);
  gst_bin_add_many(GST_BIN(pipeline), mixer, master, conv, sink, NULL);
  GstCaps *f32 = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "F32LE", NULL);
  gst_element_link_filtered(mixer, master, f32);
  gst_caps_unref(f32);
  gst_element_link_many(master, conv, sink, NULL);

  GstPad *sp = gst_element_get_static_pad(sink, "sink");
  gst_pad_add_probe(sp, GST_PAD_PROBE_TYPE_BUFFER, on_buf, NULL, NULL);

  for (int i = 0; i < n; i++) {
    GstElement *branch = gst_bin_new(NULL);
    GstElement *dec = gst_element_factory_make("uridecodebin", NULL);
    GstElement *bc = gst_element_factory_make("audioconvert", NULL);
    GstElement *rs = gst_element_factory_make("audioresample", NULL);
    GstElement *amp = gst_element_factory_make("audioamplify", NULL);
    GstElement *vol = gst_element_factory_make("volume", NULL);
    g_object_set(dec, "uri", argv[2], NULL);
    GstControlSource *cs = gst_interpolation_control_source_new();
    gst_timed_value_control_source_set(GST_TIMED_VALUE_CONTROL_SOURCE(cs), 0, 1.0);
    gst_object_add_control_binding(
        GST_OBJECT(vol), gst_direct_control_binding_new_absolute(GST_OBJECT(vol), "volume", cs));
    gst_object_unref(cs);
    gst_bin_add_many(GST_BIN(branch), dec, bc, rs, amp, vol, NULL);
    gst_element_link_many(bc, rs, amp, vol, NULL);
    g_signal_connect(dec, "pad-added", G_CALLBACK(on_pad_added), bc);
    GstPad *vs = gst_element_get_static_pad(vol, "src");
    gst_element_add_pad(branch, gst_ghost_pad_new("src", vs));
    gst_object_unref(vs);
    gst_bin_add(GST_BIN(pipeline), branch);
    GstPad *bs = gst_element_get_static_pad(branch, "src");
    gst_pad_link(bs, gst_element_request_pad_simple(mixer, "sink_%u"));
    gst_object_unref(bs);
    gst_element_sync_state_with_parent(branch);
  }

  loop = g_main_loop_new(NULL, FALSE);
  GstBus *bus = gst_element_get_bus(pipeline);
  gst_bus_add_watch(bus, on_bus, NULL);
  gst_object_unref(bus);
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  gst_element_get_state(pipeline, NULL, NULL, 10 * GST_SECOND);
  GstCaps *c = gst_pad_get_current_caps(sp);
  char *cs = c ? gst_caps_to_string(c) : g_strdup("(none)");
  g_main_loop_run(loop);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  printf("peak %.5f  over-FS %" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT "  sink caps: %s\n", peak,
         over, total, cs);
  return over != 0 || total == 0;
}
EOF

gcc -O2 -o "$D/h" "$D/h.c" $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-controller-1.0) -lm

fail=0
for fmt in S16LE F32LE; do
    for n in 3 8; do
        printf '%-6s %d tracks: ' "$fmt" "$n"
        if "$D/h" "$n" "file://$D/$fmt.wav" "$HEADROOM"; then
            echo "  -> PASS"
        else
            echo "  -> FAIL (samples at or over full scale)"
            fail=1
        fi
    done
done
exit $fail
