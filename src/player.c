#include "player.h"

#include <gst/controller/gstdirectcontrolbinding.h>
#include <gst/controller/gstinterpolationcontrolsource.h>

#define INVERT_RAMP (30 * GST_MSECOND)

// `volume` indexes its control source by stream time -- it syncs at
// gst_segment_to_stream_time(PTS), which is what player_position() reports --
// not by pipeline running time. The two only agree while playing straight
// through from 0, so audible switches are scheduled off the position.

// Declick only; `volume` interpolates the control source per sample.
#define SWITCH_RAMP (5 * GST_MSECOND)

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
    pipeline         = gst_pipeline_new("player");
    mixer            = gst_element_factory_make("audiomixer", "mix");
    GstElement *conv = gst_element_factory_make("audioconvert", NULL);
    GstElement *sink = gst_element_factory_make("autoaudiosink", NULL);
    gst_bin_add_many(GST_BIN(pipeline), mixer, conv, sink, NULL);
    gst_element_link_many(mixer, conv, sink, NULL);

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

// Stream time to schedule the next audible-set change at. Read once per
// apply_audible() sweep so every track crosses on the same sample.
GstClockTime
player_switch_time(void)
{
    gint64 pos = player_position();
    if (pos < 0)
        pos = 0;
    // No clock means the pipeline has never played, so nothing is downstream of
    // `volume` yet and the change can land at the current position.
    GstClock *clock = gst_element_get_clock(pipeline);
    if (!clock)
        return (GstClockTime)pos;
    gst_object_unref(clock);
    return (GstClockTime)pos + SWITCH_LEAD;
}

// Switch a track's audibility at stream time `when`, from player_switch_time().
void
player_set_audible(Track *t, gboolean audible, GstClockTime when)
{
    if (!t->vol_cs)
        return;
    GstTimedValueControlSource *tv = GST_TIMED_VALUE_CONTROL_SOURCE(t->vol_cs);
    double                      to = audible ? 1.0 : 0.0, from = 0.0;
    // The last point always carries the gain this function commanded last time,
    // so reading past the end of the source gives the track's current audible
    // state regardless of where playback is -- no shadow copy needed.
    gst_control_source_get_value(t->vol_cs, G_MAXINT64, &from);

    // Rewrite the whole source every time so points cannot accumulate into an
    // automation curve. `pos` is where playback is now; the five points give:
    //   [0, pos)   `to`   -- a backward seek or the EOS loop lands on the
    //                        current audible set, not on an old switch
    //   [pos, when) `from` -- buffers already past `volume` play unchanged
    //   when..+RAMP        -- the declick ramp
    //   after when+RAMP `to`
    // unset_all() first leaves the source empty for an instant, which is safe:
    // the binding holds the property at `from` meanwhile, which is what the
    // buffers in flight want. Leaving it *permanently* empty is the hazard, and
    // the sets below rule that out.
    GstClockTime pos = when > SWITCH_LEAD ? when - SWITCH_LEAD : 0;
    gst_timed_value_control_source_unset_all(tv);
    gst_timed_value_control_source_set(tv, 0, to);
    gst_timed_value_control_source_set(tv, pos > SWITCH_RAMP ? pos - SWITCH_RAMP : 0, to);
    gst_timed_value_control_source_set(tv, pos, from);
    gst_timed_value_control_source_set(tv, when, from);
    gst_timed_value_control_source_set(tv, when + SWITCH_RAMP, to);
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
