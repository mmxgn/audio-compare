#include "track.h"

#include <ebur128.h>
#include <gio/gio.h>
#include <gst/app/gstappsink.h>
#include <math.h>

#define SAMPLES_PER_PEAK 512
#define ANALYZE_RATE     48000

// ponytail: blocking decode on the calling thread. TDD open question --
// move to a worker if large files stall the UI.
static void extract_peaks(Track *t)
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

    // ponytail: loudness measured on the mono downmix -- consistent across
    // files, not the per-channel R128 spec. Good enough for comparing by ear.
    ebur128_state *r128 =
        ebur128_init(1, ANALYZE_RATE, EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);

    float cmin = 0, cmax = 0;
    int   count = 0;
    for (;;) {
        GstSample *sample = gst_app_sink_pull_sample(sink);
        if (!sample)
            break; // EOS or error
        GstBuffer *buf = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
            float *data = (float *) map.data;
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

    double peak = 0;
    ebur128_loudness_global(r128, &t->lufs);
    ebur128_true_peak(r128, 0, &peak);
    t->dbtp = peak > 0 ? 20.0 * log10(peak) : -HUGE_VAL;
    ebur128_destroy(&r128);

    gst_object_unref(sink);
    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(pipe);
}

// Loop: seek back to the start when playback reaches the end.
static gboolean on_bus(GstBus *bus, GstMessage *msg, gpointer user)
{
    Track *t = user;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS)
        gst_element_seek_simple(t->pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, 0);
    return TRUE;
}

Track *track_new(const char *uri)
{
    Track *t    = g_new0(Track, 1);
    t->uri      = g_strdup(uri);
    t->peaks    = g_array_new(FALSE, FALSE, sizeof(Peak));
    t->duration = -1;
    t->lufs     = -HUGE_VAL;
    t->dbtp     = -HUGE_VAL;

    GFile *f = g_file_new_for_uri(uri);
    t->name  = g_file_get_basename(f);
    g_object_unref(f);

    extract_peaks(t);

    t->pipeline = gst_element_factory_make("playbin", NULL);
    g_object_set(t->pipeline, "uri", t->uri, NULL);

    GstBus *bus   = gst_element_get_bus(t->pipeline);
    t->bus_watch  = gst_bus_add_watch(bus, on_bus, t);
    gst_object_unref(bus);

    // Preroll to PAUSED so duration is queryable.
    gst_element_set_state(t->pipeline, GST_STATE_PAUSED);
    gst_element_get_state(t->pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
    gst_element_query_duration(t->pipeline, GST_FORMAT_TIME, &t->duration);

    return t;
}

void track_free(Track *t)
{
    if (!t)
        return;
    if (t->bus_watch)
        g_source_remove(t->bus_watch);
    gst_element_set_state(t->pipeline, GST_STATE_NULL);
    gst_object_unref(t->pipeline);
    g_array_free(t->peaks, TRUE);
    g_free(t->name);
    g_free(t->uri);
    g_free(t);
}

void track_play(Track *t)
{
    gst_element_set_state(t->pipeline, GST_STATE_PLAYING);
}

void track_pause(Track *t)
{
    gst_element_set_state(t->pipeline, GST_STATE_PAUSED);
}

gint64 track_position(Track *t)
{
    gint64 pos = -1;
    gst_element_query_position(t->pipeline, GST_FORMAT_TIME, &pos);
    return pos;
}

void track_seek(Track *t, gint64 pos)
{
    gst_element_seek_simple(t->pipeline, GST_FORMAT_TIME,
                            GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, pos);
}
