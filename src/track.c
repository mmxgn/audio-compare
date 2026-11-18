#include "track.h"

#include <ebur128.h>
#include <gio/gio.h>
#include <gst/app/gstappsink.h>
#include <math.h>

#define SAMPLES_PER_PEAK 512
#define ANALYZE_RATE 48000

// Analysis limits. Without them a tiny, highly compressible file (hours of
// constant-value FLAC) or a source that never ends decoded into RAM as fast as
// it could, and a source that stalled without EOS blocked the UI forever.
// Decoded-but-unanalysed audio is capped at a few buffers (the decoder waits,
// nothing is dropped), analysis stops after MAX_ANALYZE_SECONDS, and a source
// that produces nothing for STALL_TIMEOUT is abandoned.
#define MAX_QUEUED_BUFFERS 16
#define MAX_ANALYZE_SECONDS (4 * 60 * 60)
#define MAX_ANALYZE_FRAMES ((guint64)MAX_ANALYZE_SECONDS * ANALYZE_RATE)
#define STALL_TIMEOUT (5 * GST_SECOND)
#define PREROLL_TIMEOUT (5 * GST_SECOND)
// Analysis runs on a worker thread; it waits in slices this short so closing
// the pane or quitting stops it promptly. Independent of STALL_TIMEOUT, which
// is measured from the last buffer actually received.
#define ANALYZE_POLL (250 * GST_MSECOND)

// The pipeline description is a constant: the URI goes in as a typed property,
// never as parse-launch text. Formatting it into the description let a quote in
// the URI close the uri= value and splice arbitrary elements into the pipeline.
// Do not "fix" that by escaping -- the grammar has more metacharacters than `"`.
GstElement *
track_analysis_pipeline(const char *uri, GstAppSink **sink_out)
{
    GError     *err = NULL;
    GstElement *pipe
        = gst_parse_launch_full("uridecodebin name=src ! audioconvert ! audioresample ! "
                                "audio/x-raw,format=F32LE,rate=" G_STRINGIFY(
                                    ANALYZE_RATE) " ! appsink name=sink sync=false",
                                NULL, GST_PARSE_FLAG_FATAL_ERRORS, &err);
    if (!pipe) {
        g_warning("peaks: %s", err ? err->message : "parse failed");
        g_clear_error(&err);
        return NULL;
    }
    GstElement *src = gst_bin_get_by_name(GST_BIN(pipe), "src");
    g_object_set(src, "uri", uri, NULL); // a value, never syntax
    gst_object_unref(src);
    *sink_out = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipe), "sink"));
    // Backpressure: block the decoder once a few buffers are waiting. drop
    // stays FALSE, so no audio is skipped -- the decoder just waits for us.
    gst_app_sink_set_max_buffers(*sink_out, MAX_QUEUED_BUFFERS);
    gst_app_sink_set_drop(*sink_out, FALSE);
    return pipe;
}

static TrackAnalysis *
analysis_new(void)
{
    TrackAnalysis *a = g_new0(TrackAnalysis, 1);
    a->peaks         = g_array_new(FALSE, FALSE, sizeof(Peak));
    a->duration      = -1;
    a->lufs          = -HUGE_VAL;
    a->dbtp          = -HUGE_VAL;
    return a;
}

void
track_analysis_free(TrackAnalysis *a)
{
    if (!a)
        return;
    g_array_free(a->peaks, TRUE);
    g_free(a);
}

// Runs on a worker thread (see track_analysis_start()): it owns `uri` and the
// pipeline it builds, and touches no Track, player or widget.
TrackAnalysis *
track_analysis_run(const char *uri, GCancellable *cancel)
{
    TrackAnalysis *a    = analysis_new();
    GstAppSink    *sink = NULL;
    GstElement    *pipe = track_analysis_pipeline(uri, &sink);
    if (!pipe)
        return a;

    gst_element_set_state(pipe, GST_STATE_PLAYING);

    // Bounded wait: pulling below waits forever if the appsink never prerolls,
    // so reject anything that does not reach PLAYING. A broken or missing file
    // fails the state change; one with no audio stream (video, image) leaves it
    // ASYNC because nothing ever links to the sink. Waited for in short slices
    // so a cancel (pane closed) is noticed promptly.
    GstStateChangeReturn ret      = GST_STATE_CHANGE_ASYNC;
    gint64               deadline = g_get_monotonic_time() + PREROLL_TIMEOUT / GST_USECOND;
    while (ret == GST_STATE_CHANGE_ASYNC && !g_cancellable_is_cancelled(cancel)
           && g_get_monotonic_time() < deadline)
        ret = gst_element_get_state(pipe, NULL, NULL, ANALYZE_POLL);

    if (ret == GST_STATE_CHANGE_SUCCESS)
        track_analyze(a, uri, pipe, sink, MAX_ANALYZE_FRAMES, STALL_TIMEOUT, cancel);
    else if (!g_cancellable_is_cancelled(cancel))
        g_warning("peaks: cannot decode %s", uri);

    gst_object_unref(sink);
    gst_element_set_state(pipe, GST_STATE_NULL); // also unblocks a full appsink
    gst_object_unref(pipe);
    return a;
}

static void
analysis_thread(GTask *task, gpointer src, gpointer uri, GCancellable *cancel)
{
    TrackAnalysis *a = track_analysis_run(uri, cancel);
    if (g_task_return_error_if_cancelled(task)) {
        track_analysis_free(a);
        return;
    }
    g_task_return_pointer(task, a, (GDestroyNotify)track_analysis_free);
}

void
track_analysis_start(const char *uri, GCancellable *cancel, GAsyncReadyCallback cb, gpointer user)
{
    GTask *task = g_task_new(NULL, cancel, cb, user);
    g_task_set_source_tag(task, track_analysis_start);
    g_task_set_task_data(task, g_strdup(uri), g_free);
    g_task_run_in_thread(task, analysis_thread);
    g_object_unref(task);
}

TrackAnalysis *
track_analysis_finish(GAsyncResult *res, GError **err)
{
    // GTask checks the cancellable here too, so a result that raced a cancel is
    // freed and reported as cancelled: the caller's Track may already be gone.
    return g_task_propagate_pointer(G_TASK(res), err);
}

// Pull decoded audio from a running analysis pipeline into a: peaks, loudness,
// true peak and duration. Stops at EOS, an error, `max_frames`, a cancel, or
// once nothing arrives for `stall_timeout`, and records why in a->truncated /
// a->stalled.
void
track_analyze(TrackAnalysis *a, const char *uri, GstElement *pipe, GstAppSink *sink,
              guint64 max_frames, GstClockTime stall_timeout, GCancellable *cancel)
{
    // R128 is defined per channel, and the channel count only arrives on the caps
    // of the first sample, so the state is created lazily in the loop.
    ebur128_state *r128     = NULL;
    int            channels = 0;
    GstBus        *bus      = gst_element_get_bus(pipe);
    GstMessage    *error    = NULL;

    GstClockTime poll  = MIN(stall_timeout, ANALYZE_POLL);
    gint64       quiet = g_get_monotonic_time() + stall_timeout / GST_USECOND; // stall deadline

    float   cmin = 0, cmax = 0;
    int     count = 0;
    guint64 total = 0; // frames analysed
    for (;;) {
        if (g_cancellable_is_cancelled(cancel))
            break;
        GstSample *sample = gst_app_sink_try_pull_sample(sink, poll);
        if (!sample) {
            // NULL is EOS, an error, or this poll's timeout. An error does not
            // make the appsink EOS, so look for it on the bus each empty poll.
            if (gst_app_sink_is_eos(sink))
                break;
            error = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
            if (error)
                break;
            if (g_get_monotonic_time() >= quiet) {
                a->stalled = TRUE;
                break;
            }
            continue;
        }
        quiet = g_get_monotonic_time() + stall_timeout / GST_USECOND;

        int           ch   = 0;
        GstCaps      *caps = gst_sample_get_caps(sample);
        GstStructure *s    = caps ? gst_caps_get_structure(caps, 0) : NULL;
        if (!s || !gst_structure_get_int(s, "channels", &ch) || ch < 1) {
            gst_sample_unref(sample);
            continue;
        }
        if (!r128) {
            r128 = ebur128_init(ch, ANALYZE_RATE, EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);
            if (!r128) {
                g_warning("peaks: ebur128_init failed");
                gst_sample_unref(sample);
                break;
            }
            channels = ch;
        }
        // ponytail: folding a mid-stream layout change into one measurement would
        // need a second R128 state; drop those buffers, real files do not do it.
        if (ch != channels) {
            gst_sample_unref(sample);
            continue;
        }

        GstBuffer *buf = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
            float  *data   = (float *)map.data;
            guint64 frames = map.size / sizeof(float) / channels;
            if (frames >= max_frames - total) {
                frames       = max_frames - total;
                a->truncated = TRUE;
            }
            total += frames;
            ebur128_add_frames_float(r128, data, frames);
            for (guint i = 0; i < frames; i++) {
                // The waveform wants one min/max pair per bucket, so collapse the
                // channels to their envelope. Not a downmix: that draws an
                // out-of-phase file as a flat line.
                for (int c = 0; c < channels; c++) {
                    float v = data[i * channels + c];
                    if (count == 0 && c == 0) {
                        cmin = cmax = v;
                    } else {
                        if (v < cmin)
                            cmin = v;
                        if (v > cmax)
                            cmax = v;
                    }
                }
                if (++count >= SAMPLES_PER_PEAK) {
                    Peak p = { cmin, cmax };
                    g_array_append_val(a->peaks, p);
                    count = 0;
                }
            }
            gst_buffer_unmap(buf, &map);
        }
        gst_sample_unref(sample);
        if (a->truncated)
            break;
    }
    if (count > 0) {
        Peak p = { cmin, cmax };
        g_array_append_val(a->peaks, p);
    }

    // pull_sample() returns NULL on a mid-file decode abort too, not just EOS;
    // the bus tells them apart. ponytail: only catches aborts that actually post
    // an error -- a plainly truncated file looks like clean EOS to GStreamer. We
    // keep the partial peaks (and the short duration derived from them) and warn.
    if (!error)
        error = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    if (error) {
        g_warning("peaks: decode of %s stopped early, waveform is incomplete", uri);
        gst_message_unref(error);
        a->stalled = FALSE; // it errored rather than stalled; already reported
    } else if (a->stalled) {
        g_warning("peaks: decode of %s stalled, waveform is incomplete", uri);
    }
    gst_object_unref(bus);

    if (r128) {
        ebur128_loudness_global(r128, &a->lufs);
        double peak = 0, p = 0;
        for (int c = 0; c < channels; c++)
            if (ebur128_true_peak(r128, c, &p) == EBUR128_SUCCESS && p > peak)
                peak = p;
        a->dbtp = peak > 0 ? 20.0 * log10(peak) : -HUGE_VAL;
        ebur128_destroy(&r128);
    }

    // Duration from the decoded peak count (one peak per 512 frames at 48 kHz),
    // so the playhead lines up with the drawn peaks. Scaled with a wide
    // intermediate: the plain product overflowed gint64 past ~53 hours.
    if (a->peaks->len > 0)
        a->duration = (gint64)gst_util_uint64_scale((guint64)a->peaks->len * SAMPLES_PER_PEAK,
                                                    GST_SECOND, ANALYZE_RATE);
}

Track *
track_new(const char *uri)
{
    Track *t     = g_new0(Track, 1);
    t->uri       = g_strdup(uri);
    t->peaks     = g_array_new(FALSE, FALSE, sizeof(Peak));
    t->duration  = -1;
    t->lufs      = -HUGE_VAL;
    t->dbtp      = -HUGE_VAL;
    t->bus       = -1;
    t->analysing = TRUE;
    t->cancel    = g_cancellable_new();

    GFile *f = g_file_new_for_uri(uri);
    t->name  = g_file_get_basename(f);
    g_object_unref(f);
    return t;
}

void
track_free(Track *t)
{
    if (!t)
        return;
    // Stop a running analysis; its completion then reports a cancel and leaves
    // this (freed) Track alone.
    g_cancellable_cancel(t->cancel);
    g_object_unref(t->cancel);
    g_array_free(t->peaks, TRUE);
    g_free(t->name);
    g_free(t->uri);
    g_free(t);
}
