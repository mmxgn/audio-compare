#include "player.h"

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
    GstElement *vol      = gst_element_factory_make("volume", NULL);

    g_object_set(dec, "uri", t->uri, NULL);
    g_object_set(vol, "volume", 0.0, NULL); // muted until made audible

    gst_bin_add_many(GST_BIN(branch), dec, conv, resample, vol, NULL);
    gst_element_link_many(conv, resample, vol, NULL);
    g_signal_connect(dec, "pad-added", G_CALLBACK(on_pad_added), conv);

    // Expose the branch output as a ghost pad and link it to the mixer.
    GstPad *src = gst_element_get_static_pad(vol, "src");
    gst_element_add_pad(branch, gst_ghost_pad_new("src", src));
    gst_object_unref(src);

    gst_bin_add(GST_BIN(pipeline), branch);
    t->branch = branch;
    t->vol    = vol;
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
    // is rare, so the brief re-preroll gap is acceptable (see plan).
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

    if (state == GST_STATE_PLAYING || state == GST_STATE_PAUSED) {
        gst_element_set_state(pipeline, state);
        if (pos > 0)
            player_seek(pos);
    }
}

void
player_set_audible(Track *t, gboolean audible)
{
    if (t->vol)
        g_object_set(t->vol, "volume", audible ? 1.0 : 0.0, NULL);
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
