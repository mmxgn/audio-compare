// Checks on the real track.c analysis pipeline (check.c stubs it out).
#include "track.h"

#include <assert.h>
#include <glib/gstdio.h>
#include <gst/gst.h>

// A quote in the URI must stay inside the uri property, never become pipeline
// syntax: no element beyond the fixed chain may be created.
static void
test_uri_is_not_pipeline_syntax(void)
{
    const char *hostile = "file:///tmp/a\" ! identity name=canary ! fakesink name=\"b";
    GstAppSink *sink    = NULL;
    GstElement *pipe    = track_analysis_pipeline(hostile, &sink);
    assert(pipe && sink);

    GstElement *canary = gst_bin_get_by_name(GST_BIN(pipe), "canary");
    assert(!canary && "injected element must not exist");
    // uridecodebin, audioconvert, audioresample, capsfilter, appsink
    assert(GST_BIN_NUMCHILDREN(pipe) == 5);

    GstElement *src = gst_bin_get_by_name(GST_BIN(pipe), "src");
    char       *uri = NULL;
    g_object_get(src, "uri", &uri, NULL);
    assert(g_str_equal(uri, hostile) && "the URI is carried verbatim as a value");
    g_free(uri);
    gst_object_unref(src);

    gst_object_unref(sink);
    gst_object_unref(pipe);
}

// Regression: a real file whose name is full of parse-launch metacharacters
// still decodes, since its URI comes from g_filename_to_uri().
static void
test_odd_filename_still_decodes(void)
{
    char *dir  = g_dir_make_tmp("ac-analysis-XXXXXX", NULL);
    char *path = g_build_filename(dir, "take \"2\" ! #50% é.wav", NULL);

    GstElement *gen
        = gst_parse_launch("audiotestsrc num-buffers=50 ! wavenc ! filesink name=out", NULL);
    GstElement *out = gst_bin_get_by_name(GST_BIN(gen), "out");
    g_object_set(out, "location", path, NULL);
    gst_object_unref(out);
    gst_element_set_state(gen, GST_STATE_PLAYING);
    GstBus     *bus = gst_element_get_bus(gen);
    GstMessage *msg
        = gst_bus_timed_pop_filtered(bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    assert(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS);
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(gen, GST_STATE_NULL);
    gst_object_unref(gen);

    char  *uri = g_filename_to_uri(path, NULL, NULL);
    Track *t   = track_new(uri);
    assert(t->peaks->len > 0 && "the file decodes");
    assert(t->duration > 0);
    track_free(t);
    g_free(uri);

    g_unlink(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
}

// Runs `desc` (ending in "appsink name=sink") through track_analyze() with the
// given limits and returns the Track; *elapsed gets the wall time taken.
static Track *
analyze_desc(const char *desc, guint64 max_frames, GstClockTime stall, gint64 *elapsed)
{
    Track *t    = g_new0(Track, 1);
    t->uri      = g_strdup("test://");
    t->peaks    = g_array_new(FALSE, FALSE, sizeof(Peak));
    t->duration = -1;

    GstElement *pipe = gst_parse_launch(desc, NULL);
    assert(pipe);
    GstAppSink *sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipe), "sink"));
    gst_app_sink_set_max_buffers(sink, 16);
    gst_element_set_state(pipe, GST_STATE_PLAYING);

    gint64 t0 = g_get_monotonic_time();
    track_analyze(t, pipe, sink, max_frames, stall);
    *elapsed = g_get_monotonic_time() - t0;

    gst_object_unref(sink);
    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(pipe);
    return t;
}

// A source that never ends must stop at the length cap, flagged, with exactly
// the capped number of frames analysed.
static void
test_endless_source_is_capped(void)
{
    gint64 us;
    Track *t = analyze_desc(
        "audiotestsrc is-live=false ! audio/x-raw,format=F32LE,rate=48000,channels=2 "
        "! appsink name=sink sync=false",
        3 * 48000, 5 * GST_SECOND, &us);
    assert(t->truncated && !t->stalled);
    assert(t->peaks->len == (3 * 48000 + 511) / 512);
    assert(t->duration > 2 * GST_SECOND && t->duration < 4 * GST_SECOND);
    track_free(t);
}

// A source that stops producing without EOS must be abandoned after the stall
// timeout, flagged, instead of blocking forever.
static void
test_stalled_source_times_out(void)
{
    gint64 us;
    // An appsrc nobody feeds: negotiated, never produces, never sends EOS.
    Track *t
        = analyze_desc("appsrc name=src is-live=true format=time "
                       "caps=audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved "
                       "! appsink name=sink sync=false",
                       G_MAXUINT64, GST_SECOND / 2, &us);
    assert(t->stalled && !t->truncated);
    assert(us < 3 * G_USEC_PER_SEC && "returned near the stall timeout");
    track_free(t);
}

int
main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    test_uri_is_not_pipeline_syntax();
    test_odd_filename_still_decodes();
    test_endless_source_is_capped();
    test_stalled_source_times_out();
    g_print("analysis checks passed\n");
    return 0;
}
