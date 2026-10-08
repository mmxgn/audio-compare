#!/usr/bin/env sh
# Asserts that a mid-file seek leaves every branch on the same audio sample.
#
# qtdemux and oggdemux honour GST_SEEK_FLAG_KEY_UNIT by snapping to the nearest
# sync point, which silently lands two tracks at different media positions and
# destroys the null test the app exists for. wav and mp3 never snapped, so they
# are here only as a regression check.
#
# Run inside `nix develop`. Build a reference by decoding each file linearly
# through the app's branch chain, then seek to 10 s and null it against the
# same file played through a second, inverted branch.
set -e
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT

cat >"$d/n.c" <<'EOF'
// Replica of the app's mixer chain: two branches into one audiomixer, branch B
// inverted so an aligned pair sums to silence. B == "-" means "same file at
// amplification 0", i.e. identical topology with no cancellation, which with
// [out.wav] captures the branch's own output as a reference.
//   n A B seek_ms none|keyunit|accurate measure_ms [out.wav]
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define RATE 44100
static GstElement *pipeline, *mixer;
static void
on_pad_added(GstElement *d, GstPad *pad, gpointer user)
{
    GstPad *s = gst_element_get_static_pad(GST_ELEMENT(user), "sink");
    if (!gst_pad_is_linked(s))
        gst_pad_link(pad, s);
    gst_object_unref(s);
}
static void
add_branch(const char *path, double amp_v)
{
    GstElement *branch = gst_bin_new(NULL);
    GstElement *dec    = gst_element_factory_make("uridecodebin", NULL);
    GstElement *conv   = gst_element_factory_make("audioconvert", NULL);
    GstElement *rs     = gst_element_factory_make("audioresample", NULL);
    GstElement *amp    = gst_element_factory_make("audioamplify", NULL);
    GstElement *vol    = gst_element_factory_make("volume", NULL);
    gchar      *uri    = gst_filename_to_uri(path, NULL);
    g_object_set(dec, "uri", uri, NULL);
    g_free(uri);
    g_object_set(amp, "amplification", amp_v, NULL);
    gst_bin_add_many(GST_BIN(branch), dec, conv, rs, amp, vol, NULL);
    gst_element_link_many(conv, rs, amp, vol, NULL);
    g_signal_connect(dec, "pad-added", G_CALLBACK(on_pad_added), conv);
    GstPad *src = gst_element_get_static_pad(vol, "src");
    gst_element_add_pad(branch, gst_ghost_pad_new("src", src));
    gst_object_unref(src);
    gst_bin_add(GST_BIN(pipeline), branch);
    GstPad *mp = gst_element_request_pad_simple(mixer, "sink_%u");
    GstPad *bs = gst_element_get_static_pad(branch, "src");
    gst_pad_link(bs, mp);
    gst_object_unref(bs);
}
// Minimal 44-byte IEEE-float WAVE header; rewritten with the real size on close.
static void
wav_header(FILE *f, unsigned bytes)
{
    unsigned       h32[] = {36 + bytes, 16, RATE, RATE * 8, bytes};
    unsigned short h16[] = {3, 2, 8, 32};
    fwrite("RIFF", 1, 4, f);
    fwrite(&h32[0], 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&h32[1], 4, 1, f);
    fwrite(&h16[0], 2, 2, f);
    fwrite(&h32[2], 4, 2, f);
    fwrite(&h16[2], 2, 2, f);
    fwrite("data", 1, 4, f);
    fwrite(&h32[4], 4, 1, f);
}
int
main(int argc, char **argv)
{
    if (argc < 6)
        return 2;
    gst_init(&argc, &argv);
    const char  *a = argv[1], *b = argv[2], *mode = argv[4];
    gint64       seek_ns = g_ascii_strtoll(argv[3], NULL, 10) * GST_MSECOND;
    GstSeekFlags flags   = GST_SEEK_FLAG_FLUSH;
    if (!strcmp(mode, "keyunit"))
        flags |= GST_SEEK_FLAG_KEY_UNIT;
    else if (!strcmp(mode, "accurate"))
        flags |= GST_SEEK_FLAG_ACCURATE;

    pipeline          = gst_pipeline_new("t");
    mixer             = gst_element_factory_make("audiomixer", NULL);
    GstElement *conv  = gst_element_factory_make("audioconvert", NULL);
    GstElement *caps  = gst_element_factory_make("capsfilter", NULL);
    GstElement *sink  = gst_element_factory_make("appsink", NULL);
    GstCaps    *ocaps = gst_caps_from_string("audio/x-raw,format=F32LE,rate=44100,channels=2,"
                                             "layout=interleaved");
    g_object_set(caps, "caps", ocaps, NULL);
    gst_caps_unref(ocaps);
    g_object_set(sink, "sync", FALSE, "max-buffers", 8, NULL);
    gst_bin_add_many(GST_BIN(pipeline), mixer, conv, caps, sink, NULL);
    gst_element_link_many(mixer, conv, caps, sink, NULL);
    add_branch(a, 1.0);
    add_branch(!strcmp(b, "-") ? a : b, !strcmp(b, "-") ? 0.0 : -1.0);

    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    if (gst_element_get_state(pipeline, NULL, NULL, 20 * GST_SECOND) != GST_STATE_CHANGE_SUCCESS) {
        g_printerr("preroll failed\n");
        return 1;
    }
    gint64 t0 = g_get_monotonic_time();
    if (strcmp(mode, "none")
        && !gst_element_seek_simple(pipeline, GST_FORMAT_TIME, flags, seek_ns)) {
        g_printerr("seek failed\n");
        return 1;
    }
    gst_element_get_state(pipeline, NULL, NULL, 20 * GST_SECOND);
    double seek_ms = (g_get_monotonic_time() - t0) / 1000.0;
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    FILE    *out   = argc > 6 ? fopen(argv[6], "wb") : NULL;
    unsigned nbyte = 0;
    double   sumsq = 0;
    guint64  n = 0,
            want = gst_util_uint64_scale(g_ascii_strtoll(argv[5], NULL, 10) * GST_MSECOND, RATE,
                                         GST_SECOND);
    if (out)
        wav_header(out, 0);
    while (n < want) {
        GstSample *s = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        if (!s)
            break;
        GstBuffer *buf = gst_sample_get_buffer(s);
        GstMapInfo m;
        if (gst_buffer_map(buf, &m, GST_MAP_READ)) {
            float *f = (float *)m.data;
            for (gsize i = 0; i < m.size / sizeof(float); i++)
                sumsq += (double)f[i] * f[i];
            if (out) {
                fwrite(m.data, 1, m.size, out);
                nbyte += m.size;
            }
            n += m.size / (sizeof(float) * 2);
            gst_buffer_unmap(buf, &m);
        }
        gst_sample_unref(s);
    }
    if (out) {
        fseek(out, 0, SEEK_SET);
        wav_header(out, nbyte);
        fclose(out);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    double rms = n ? sqrt(sumsq / (double)(n * 2)) : 0;
    g_print("%.1f %.2f\n", rms > 1e-12 ? 20 * log10(rms) : -240.0, seek_ms);
    return 0;
}
EOF
cc -O2 "$d/n.c" -o "$d/n" $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0) -lm

# 20 s of pink noise, encoded to every format the app opens.
gst-launch-1.0 -q audiotestsrc wave=pink-noise num-buffers=862 samplesperbuffer=1024 \
  ! audio/x-raw,rate=44100,channels=2,format=S16LE ! wavenc ! filesink location="$d/t.wav"
gst-launch-1.0 -q filesrc location="$d/t.wav" ! wavparse ! audioconvert \
  ! fdkaacenc bitrate=192000 ! mp4mux ! filesink location="$d/t.m4a"
gst-launch-1.0 -q filesrc location="$d/t.wav" ! wavparse ! audioconvert \
  ! vorbisenc quality=0.8 ! oggmux ! filesink location="$d/t.ogg"
gst-launch-1.0 -q filesrc location="$d/t.wav" ! wavparse ! audioconvert \
  ! lamemp3enc ! filesink location="$d/t.mp3"

fail=0
for f in m4a ogg wav mp3; do
    "$d/n" "$d/t.$f" - 0 none 15000 "$d/ref_$f.wav" >/dev/null
    set -- $("$d/n" "$d/ref_$f.wav" "$d/t.$f" 10000 accurate 2000)
    verdict=$(awk -v r="$1" 'BEGIN { print (r < -60) ? "PASS" : "FAIL" }')
    printf '%-4s residual %8s dBFS   seek %5s ms   %s\n' "$f" "$1" "$2" "$verdict"
    [ "$verdict" = PASS ] || fail=1
done
exit $fail
