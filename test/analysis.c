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

    char          *uri = g_filename_to_uri(path, NULL, NULL);
    TrackAnalysis *a   = track_analysis_run(uri, NULL);
    assert(a->peaks->len > 0 && "the file decodes");
    assert(a->duration > 0);
    track_analysis_free(a);
    g_free(uri);

    g_unlink(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
}

// Runs `desc` (ending in "appsink name=sink") through track_analyze() with the
// given limits and returns the result; *elapsed gets the wall time taken.
static TrackAnalysis *
analyze_desc(const char *desc, guint64 max_frames, GstClockTime stall, GCancellable *cancel,
             gint64 *elapsed)
{
    TrackAnalysis *t = g_new0(TrackAnalysis, 1);
    t->peaks         = g_array_new(FALSE, FALSE, sizeof(Peak));
    t->duration      = -1;

    GstElement *pipe = gst_parse_launch(desc, NULL);
    assert(pipe);
    GstAppSink *sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipe), "sink"));
    gst_app_sink_set_max_buffers(sink, 16);
    gst_element_set_state(pipe, GST_STATE_PLAYING);

    gint64 t0 = g_get_monotonic_time();
    track_analyze(t, "test://", pipe, sink, max_frames, stall, cancel);
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
    gint64         us;
    TrackAnalysis *t = analyze_desc(
        "audiotestsrc is-live=false ! audio/x-raw,format=F32LE,rate=48000,channels=2 "
        "! appsink name=sink sync=false",
        3 * 48000, 5 * GST_SECOND, NULL, &us);
    assert(t->truncated && !t->stalled);
    assert(t->peaks->len == (3 * 48000 + 511) / 512);
    assert(t->duration > 2 * GST_SECOND && t->duration < 4 * GST_SECOND);
    track_analysis_free(t);
}

// A source that stops producing without EOS must be abandoned after the stall
// timeout, flagged, instead of blocking forever.
static void
test_stalled_source_times_out(void)
{
    gint64 us;
    // An appsrc nobody feeds: negotiated, never produces, never sends EOS.
    TrackAnalysis *t
        = analyze_desc("appsrc name=src is-live=true format=time "
                       "caps=audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved "
                       "! appsink name=sink sync=false",
                       G_MAXUINT64, GST_SECOND / 2, NULL, &us);
    assert(t->stalled && !t->truncated);
    assert(us < 3 * G_USEC_PER_SEC && "returned near the stall timeout");
    track_analysis_free(t);
}

static gpointer
cancel_later(gpointer cancel)
{
    g_usleep(300 * 1000);
    g_cancellable_cancel(cancel);
    return NULL;
}

// An endless source analysed with no length cap must still stop soon after
// its cancellable fires: closing a pane mid-analysis has to end the work.
static void
test_cancel_stops_analysis(void)
{
    GCancellable *cancel = g_cancellable_new();
    GThread      *th     = g_thread_new("cancel", cancel_later, cancel);

    gint64         us;
    TrackAnalysis *t = analyze_desc(
        "audiotestsrc is-live=false ! audio/x-raw,format=F32LE,rate=48000,channels=2 "
        "! appsink name=sink sync=false",
        G_MAXUINT64, 5 * GST_SECOND, cancel, &us);
    assert(g_cancellable_is_cancelled(cancel));
    assert(!t->truncated && !t->stalled && "a cancel is neither a cap nor a stall");
    assert(us < 2 * G_USEC_PER_SEC && "returned promptly after the cancel");
    track_analysis_free(t);
    g_thread_join(th);
    g_object_unref(cancel);
}

// track_new() must not decode anything: it runs on the UI thread.
static void
test_track_new_is_cheap(void)
{
    gint64 t0 = g_get_monotonic_time();
    Track *t  = track_new("file:///nonexistent/dir/x.flac");
    assert(g_get_monotonic_time() - t0 < G_USEC_PER_SEC / 10);
    assert(t->analysing && t->peaks->len == 0 && t->duration == -1);
    assert(g_str_equal(t->name, "x.flac"));
    track_free(t);
}

typedef struct {
    gboolean       done;
    TrackAnalysis *a;
    GError        *err;
} Done;

static void
on_done(GObject *src, GAsyncResult *res, gpointer user)
{
    Done *d = user;
    d->a    = track_analysis_finish(res, &d->err);
    d->done = TRUE;
}

static char *
make_wav(char **dir_out)
{
    char       *dir  = g_dir_make_tmp("ac-async-XXXXXX", NULL);
    char       *path = g_build_filename(dir, "a.wav", NULL);
    GstElement *gen
        = gst_parse_launch("audiotestsrc num-buffers=50 ! wavenc ! filesink name=out", NULL);
    GstElement *out = gst_bin_get_by_name(GST_BIN(gen), "out");
    g_object_set(out, "location", path, NULL);
    gst_object_unref(out);
    gst_element_set_state(gen, GST_STATE_PLAYING);
    GstBus     *bus = gst_element_get_bus(gen);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 10 * GST_SECOND, GST_MESSAGE_EOS);
    assert(msg);
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(gen, GST_STATE_NULL);
    gst_object_unref(gen);
    *dir_out = dir;
    return path;
}

// The worker path end to end: a result arrives on the main context, and a
// cancel before the completion runs is reported as such even if the worker had
// already finished, so the caller never touches a freed Track.
static void
test_async_analysis(void)
{
    char *dir, *path = make_wav(&dir);
    char *uri = g_filename_to_uri(path, NULL, NULL);

    Done d = { 0 };
    track_analysis_start(uri, NULL, on_done, &d);
    while (!d.done)
        g_main_context_iteration(NULL, TRUE);
    assert(d.a && !d.err && d.a->peaks->len > 0 && d.a->duration > 0);
    track_analysis_free(d.a);

    Done          c      = { 0 };
    GCancellable *cancel = g_cancellable_new();
    track_analysis_start(uri, cancel, on_done, &c);
    g_usleep(G_USEC_PER_SEC); // let the worker finish: the cancel must still win
    g_cancellable_cancel(cancel);
    while (!c.done)
        g_main_context_iteration(NULL, TRUE);
    assert(!c.a && g_error_matches(c.err, G_IO_ERROR, G_IO_ERROR_CANCELLED));
    g_clear_error(&c.err);
    g_object_unref(cancel);

    g_free(uri);
    g_unlink(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
}

// Regression: GTK on macOS pushes a temporary thread-default main context while
// it dispatches a drag-and-drop (seen in on_drop), and that context is never
// iterated again. A completion bound to it never ran, so the pane said
// "Analysing..." forever. It must land on the default context regardless.
static void
test_completion_ignores_temporary_context(void)
{
    char *dir, *path = make_wav(&dir);
    char *uri = g_filename_to_uri(path, NULL, NULL);

    GMainContext *tmp = g_main_context_new();
    Done          d   = { 0 };
    g_main_context_push_thread_default(tmp); // as during a drop
    track_analysis_start(uri, NULL, on_done, &d);
    g_main_context_pop_thread_default(tmp);

    gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (!d.done && g_get_monotonic_time() < deadline)
        if (!g_main_context_iteration(NULL, FALSE))
            g_usleep(1000);
    assert(d.done && "completion must arrive on the default main context");
    assert(d.a && d.a->peaks->len > 0);
    track_analysis_free(d.a);
    g_main_context_unref(tmp);

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
    test_endless_source_is_capped();
    test_stalled_source_times_out();
    test_track_new_is_cheap();
    test_async_analysis();
    test_completion_ignores_temporary_context();
    test_cancel_stops_analysis();
    g_print("analysis checks passed\n");
    return 0;
}
