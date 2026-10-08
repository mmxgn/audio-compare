#include "player.h"

#include <gst/controller/gstdirectcontrolbinding.h>
#include <gst/controller/gstinterpolationcontrolsource.h>

#define INVERT_RAMP (30 * GST_MSECOND)
// Master headroom, applied once to the whole mix: -12 dB, i.e. room for four
// tracks summing coherently at full scale before the sink has to clip.
#define MIX_HEADROOM 0.25

static GstElement *pipeline;
static GstElement *mixer;
static guint       bus_watch;

// Loop the whole mix from the start when it ends.
static gboolean
on_bus(GstBus *bus, GstMessage *msg, gpointer user)
{
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS)
        gst_element_seek_simple(pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, 0);
    return TRUE;
}

// Pipeline running time in nanoseconds, or 0 if no clock is available yet.
gint64
player_running_time(void)
{
    GstClock *clock = gst_element_get_clock(pipeline);
    if (!clock)
        return 0;
    GstClockTime t = gst_clock_get_time(clock) - gst_element_get_base_time(pipeline);
    gst_object_unref(clock);
    return (gint64)t;
}

void
player_init(void)
{
    pipeline           = gst_pipeline_new("player");
    mixer              = gst_element_factory_make("audiomixer", "mix");
    GstElement *master = gst_element_factory_make("volume", "master");
    GstElement *conv   = gst_element_factory_make("audioconvert", NULL);
    GstElement *sink   = gst_element_factory_make("autoaudiosink", NULL);
    g_object_set(master, "volume", MIX_HEADROOM, NULL);
    gst_bin_add_many(GST_BIN(pipeline), mixer, master, conv, sink, NULL);

    // Force the mix itself into float: summing N tracks in the sources' native
    // S16 saturates at full scale and destroys the sum, whereas float keeps it
    // and `master` then scales the whole bus back under full scale. The same
    // attenuation is applied to every bus, so busses stay comparable to each
    // other and a bus of 3 stems is still louder than one of 1, as it should be.
    GstCaps *f32 = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "F32LE", NULL);
    gst_element_link_filtered(mixer, master, f32);
    gst_caps_unref(f32);
    gst_element_link_many(master, conv, sink, NULL);

    GstBus *bus = gst_element_get_bus(pipeline);
    bus_watch   = gst_bus_add_watch(bus, on_bus, NULL);
    gst_object_unref(bus);

    gst_element_set_state(pipeline, GST_STATE_PAUSED);
}

void
player_shutdown(void)
{
    if (!pipeline)
        return;
    if (bus_watch)
        g_source_remove(bus_watch);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    pipeline = NULL;
    mixer    = NULL;
}

// uridecodebin exposes its audio pad late; link it to the branch's converter.
static void
on_pad_added(GstElement *dec, GstPad *pad, gpointer user)
{
    GstElement *conv = user;
    GstPad     *sink = gst_element_get_static_pad(conv, "sink");
    if (!gst_pad_is_linked(sink))
        gst_pad_link(pad, sink);
    gst_object_unref(sink);
}

void
player_add(Track *t)
{
    GstElement *branch   = gst_bin_new(NULL);
    GstElement *dec      = gst_element_factory_make("uridecodebin", NULL);
    GstElement *conv     = gst_element_factory_make("audioconvert", NULL);
    GstElement *resample = gst_element_factory_make("audioresample", NULL);
    GstElement *amp      = gst_element_factory_make("audioamplify", NULL);
    GstElement *vol      = gst_element_factory_make("volume", NULL);

    g_object_set(dec, "uri", t->uri, NULL);

    // Drive both volume and polarity through control sources so every change
    // lands on a specific pipeline running-time sample, with no buffer-boundary
    // races from g_object_set.

    // vol: starts muted (0.0); player_set_audible() moves it sample-accurately.
    GstControlSource *vol_cs = gst_interpolation_control_source_new();
    g_object_set(vol_cs, "mode", GST_INTERPOLATION_MODE_LINEAR, NULL);
    gst_timed_value_control_source_set(GST_TIMED_VALUE_CONTROL_SOURCE(vol_cs), 0, 0.0);
    gst_object_add_control_binding(GST_OBJECT(vol), gst_direct_control_binding_new_absolute(
                                                        GST_OBJECT(vol), "volume", vol_cs));
    t->vol_cs = vol_cs; // borrowed; the binding owns a ref
    gst_object_unref(vol_cs);

    // amp: polarity ramp, same pattern as before.
    GstControlSource *amp_cs = gst_interpolation_control_source_new();
    g_object_set(amp_cs, "mode", GST_INTERPOLATION_MODE_LINEAR, NULL);
    gst_timed_value_control_source_set(GST_TIMED_VALUE_CONTROL_SOURCE(amp_cs), 0,
                                       t->inverted ? -1.0 : 1.0);
    gst_object_add_control_binding(GST_OBJECT(amp), gst_direct_control_binding_new_absolute(
                                                        GST_OBJECT(amp), "amplification", amp_cs));
    t->amp_cs = amp_cs; // borrowed; the binding owns a ref
    gst_object_unref(amp_cs);

    gst_bin_add_many(GST_BIN(branch), dec, conv, resample, amp, vol, NULL);
    gst_element_link_many(conv, resample, amp, vol, NULL);
    g_signal_connect(dec, "pad-added", G_CALLBACK(on_pad_added), conv);

    // Expose the branch output as a ghost pad and link it to the mixer.
    GstPad *src = gst_element_get_static_pad(vol, "src");
    gst_element_add_pad(branch, gst_ghost_pad_new("src", src));
    gst_object_unref(src);

    gst_bin_add(GST_BIN(pipeline), branch);
    t->branch = branch;
    t->vol    = vol;
    t->amp    = amp;
    t->mixpad = gst_element_request_pad_simple(mixer, "sink_%u");

    GstPad *bsrc = gst_element_get_static_pad(branch, "src");
    gst_pad_link(bsrc, t->mixpad);
    gst_object_unref(bsrc);

    gst_element_sync_state_with_parent(branch);
}

void
player_remove(Track *t)
{
    if (!t->branch)
        return;

    // Stop the pipeline so removal is race-free, then restore. Closing a track
    // is rare, so the brief re-preroll gap is acceptable.
    GstState state;
    gst_element_get_state(pipeline, &state, NULL, 0);
    gint64 pos = player_position();

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_element_get_state(pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);

    GstPad *bsrc = gst_element_get_static_pad(t->branch, "src");
    gst_pad_unlink(bsrc, t->mixpad);
    gst_object_unref(bsrc);
    gst_element_release_request_pad(mixer, t->mixpad);
    gst_object_unref(t->mixpad);
    gst_bin_remove(GST_BIN(pipeline), t->branch); // drops the pipeline's ref
    t->mixpad = NULL;
    t->branch = NULL;
    t->vol    = NULL;
    t->amp    = NULL;
    t->vol_cs = NULL; // freed with its binding when the branch is disposed
    t->amp_cs = NULL;

    if (state == GST_STATE_PLAYING || state == GST_STATE_PAUSED) {
        gst_element_set_state(pipeline, state);
        if (pos > 0)
            player_seek(pos);
    }
}

// Switch a track's audibility at exactly `when` (pipeline running time).
// Caller should pass the same `when` for all tracks in one apply_audible()
// sweep so they all flip on the same audio sample.
void
player_set_audible(Track *t, gboolean audible, GstClockTime when)
{
    if (!t->vol_cs)
        return;
    GstTimedValueControlSource *tv = GST_TIMED_VALUE_CONTROL_SOURCE(t->vol_cs);
    // Set a constant from `when` onwards. Adding the new point before removing
    // stale ones ensures the control source is never empty (no undefined read).
    gst_timed_value_control_source_set(tv, when, audible ? 1.0 : 0.0);
    // Remove any earlier points that are now superseded.
    gst_timed_value_control_source_unset(tv, 0);
}

void
player_set_inverted(Track *t, gboolean inverted)
{
    if (!t->amp_cs)
        return;
    GstTimedValueControlSource *tv = GST_TIMED_VALUE_CONTROL_SOURCE(t->amp_cs);
    double                      to = inverted ? -1.0 : 1.0;

    GstClockTime now = (GstClockTime)player_running_time();
    // Ramp from the old polarity to the new one; the element interpolates
    // per-sample through zero, so there is no click.
    gst_timed_value_control_source_unset_all(tv);
    gst_timed_value_control_source_set(tv, now, -to);
    gst_timed_value_control_source_set(tv, now + INVERT_RAMP, to);
}

void
player_play(void)
{
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

void
player_pause(void)
{
    gst_element_set_state(pipeline, GST_STATE_PAUSED);
}

void
player_seek(gint64 pos)
{
    gst_element_seek_simple(pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                            pos);
}

gint64
player_position(void)
{
    gint64 pos = -1;
    gst_element_query_position(pipeline, GST_FORMAT_TIME, &pos);
    return pos;
}
