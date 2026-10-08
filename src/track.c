#include "track.h"

#include <ebur128.h>
#include <gio/gio.h>
#include <gst/app/gstappsink.h>
#include <math.h>

#define SAMPLES_PER_PEAK 512
#define ANALYZE_RATE 48000

// ponytail: blocking decode on the calling thread. TDD open question --
// move to a worker if large files stall the UI.
static void
extract_peaks(Track *t)
{
    char *desc = g_strdup_printf(
        "uridecodebin uri=\"%s\" ! audioconvert ! audioresample ! "
        "audio/x-raw,format=F32LE,channels=1,rate=%d ! appsink name=sink sync=false",
        t->uri, ANALYZE_RATE);
    GError     *err  = NULL;
    GstElement *pipe = gst_parse_launch(desc, &err);
    g_free(desc);
    if (!pipe) {
        g_warning("peaks: %s", err ? err->message : "parse failed");
        g_clear_error(&err);
        return;
    }

    GstAppSink *sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipe), "sink"));
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

    // ponytail: loudness measured on the mono downmix -- consistent across
    // files, not the per-channel R128 spec. Good enough for comparing by ear.
    ebur128_state *r128 = ebur128_init(1, ANALYZE_RATE, EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);

    float cmin = 0, cmax = 0;
    int   count = 0;
    for (;;) {
        GstSample *sample = gst_app_sink_pull_sample(sink);
        if (!sample)
            break; // EOS or error
        GstBuffer *buf = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
            float *data = (float *)map.data;
            guint  n    = map.size / sizeof(float);
            ebur128_add_frames_float(r128, data, n);
            for (guint i = 0; i < n; i++) {
                float v = data[i];
                if (count == 0) {
                    cmin = cmax = v;
                } else {
                    if (v < cmin)
                        cmin = v;
                    if (v > cmax)
                        cmax = v;
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
    }
    gst_object_unref(bus);

    double peak = 0;
    ebur128_loudness_global(r128, &t->lufs);
    ebur128_true_peak(r128, 0, &peak);
    t->dbtp = peak > 0 ? 20.0 * log10(peak) : -HUGE_VAL;
    ebur128_destroy(&r128);

    gst_object_unref(sink);
    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(pipe);
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

    // Duration from the decoded peak count (48 kHz mono) -- no preroll needed.
    if (t->peaks->len > 0)
        t->duration = (gint64)t->peaks->len * SAMPLES_PER_PEAK * GST_SECOND / ANALYZE_RATE;

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
