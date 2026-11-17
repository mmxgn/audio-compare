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

// ponytail: blocking decode on the calling thread. TDD open question --
// move to a worker if large files stall the UI.
static void
extract_peaks(Track *t)
{
    GstAppSink *sink = NULL;
    GstElement *pipe = track_analysis_pipeline(t->uri, &sink);
    if (!pipe)
        return;

    gst_element_set_state(pipe, GST_STATE_PLAYING);

    // Bounded wait: pull_sample() below blocks forever if the appsink never
    // prerolls, so reject anything that does not reach PLAYING. A broken or
    // missing file fails the state change; one with no audio stream (video,
    // image, directory) leaves it ASYNC because nothing ever links to the sink.
    if (gst_element_get_state(pipe, NULL, NULL, 5 * GST_SECOND) != GST_STATE_CHANGE_SUCCESS) {
        g_warning("peaks: cannot decode %s", t->uri);
        gst_object_unref(sink);
        gst_element_set_state(pipe, GST_STATE_NULL);
        gst_object_unref(pipe);
        return;
    }

    track_analyze(t, pipe, sink, MAX_ANALYZE_FRAMES, STALL_TIMEOUT);

    gst_object_unref(sink);
    gst_element_set_state(pipe, GST_STATE_NULL); // also unblocks a full appsink
    gst_object_unref(pipe);
}

// Pull decoded audio from a running analysis pipeline into t: peaks, loudness,
// true peak and duration. Stops at EOS, an error, `max_frames`, or once nothing
// arrives for `stall_timeout`, and records why in t->truncated / t->stalled.
void
track_analyze(Track *t, GstElement *pipe, GstAppSink *sink, guint64 max_frames,
              GstClockTime stall_timeout)
{
    // R128 is defined per channel, and the channel count only arrives on the caps
    // of the first sample, so the state is created lazily in the loop.
    ebur128_state *r128     = NULL;
    int            channels = 0;

    float   cmin = 0, cmax = 0;
    int     count = 0;
    guint64 total = 0; // frames analysed
    for (;;) {
        GstSample *sample = gst_app_sink_try_pull_sample(sink, stall_timeout);
        if (!sample) {
            // NULL is EOS, an error, or the timeout; only the last is not EOS.
            // An error is told apart on the bus below.
            if (!gst_app_sink_is_eos(sink))
                t->stalled = TRUE;
            break;
        }

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
                t->truncated = TRUE;
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
                    g_array_append_val(t->peaks, p);
                    count = 0;
                }
            }
            gst_buffer_unmap(buf, &map);
        }
        gst_sample_unref(sample);
        if (t->truncated)
            break;
    }
    if (count > 0) {
        Peak p = { cmin, cmax };
        g_array_append_val(t->peaks, p);
    }

    // pull_sample() returns NULL on a mid-file decode abort too, not just EOS;
    // the bus tells them apart. ponytail: only catches aborts that actually post
    // an error -- a plainly truncated file looks like clean EOS to GStreamer. We
    // keep the partial peaks (and the short duration derived from them) and warn.
    GstBus     *bus = gst_element_get_bus(pipe);
    GstMessage *msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    if (msg) {
        g_warning("peaks: decode of %s stopped early, waveform is incomplete", t->uri);
        gst_message_unref(msg);
        t->stalled = FALSE; // it errored rather than stalled; already reported
    } else if (t->stalled) {
        g_warning("peaks: decode of %s stalled, waveform is incomplete", t->uri);
    }
    gst_object_unref(bus);

    if (r128) {
        ebur128_loudness_global(r128, &t->lufs);
        double peak = 0, p = 0;
        for (int c = 0; c < channels; c++)
            if (ebur128_true_peak(r128, c, &p) == EBUR128_SUCCESS && p > peak)
                peak = p;
        t->dbtp = peak > 0 ? 20.0 * log10(peak) : -HUGE_VAL;
        ebur128_destroy(&r128);
    }

    // Duration from the decoded peak count (one peak per 512 frames at 48 kHz),
    // so the playhead lines up with the drawn peaks. Scaled with a wide
    // intermediate: the plain product overflowed gint64 past ~53 hours.
    if (t->peaks->len > 0)
        t->duration = (gint64)gst_util_uint64_scale((guint64)t->peaks->len * SAMPLES_PER_PEAK,
                                                    GST_SECOND, ANALYZE_RATE);
}

Track *
track_new(const char *uri)
{
    Track *t    = g_new0(Track, 1);
    t->uri      = g_strdup(uri);
    t->peaks    = g_array_new(FALSE, FALSE, sizeof(Peak));
    t->duration = -1;
    t->lufs     = -HUGE_VAL;
    t->dbtp     = -HUGE_VAL;
    t->bus      = -1;

    GFile *f = g_file_new_for_uri(uri);
    t->name  = g_file_get_basename(f);
    g_object_unref(f);

    extract_peaks(t);
    return t;
}

void
track_free(Track *t)
{
    if (!t)
        return;
    g_array_free(t->peaks, TRUE);
    g_free(t->name);
    g_free(t->uri);
    g_free(t);
}
