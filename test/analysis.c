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

int
main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    test_uri_is_not_pipeline_syntax();
    test_odd_filename_still_decodes();
    g_print("analysis checks passed\n");
    return 0;
}
