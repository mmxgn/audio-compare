// Repro for a failed branch being torn down while a streaming thread is parked
// in the mixer: with the pipeline PAUSED and prerolled, an error posted by one
// track's decoder makes on_bus() -> release_branch() run on the main thread.
// The main loop must survive it, the branch must go, and the other track must
// still play. A watchdog thread (not a main-loop source, which could not fire
// if the loop were stuck) fails the test if the main loop hangs.
//
// Exits 77 (meson "skipped") when there is no audio sink to preroll against.
#include "player.h"
#include "track.h"

#include <assert.h>
#include <glib/gstdio.h>
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static Track      tracks[2];
static GMainLoop *loop;
static gint       survived;
static int        errors_reported;
static gint64     pos_before;

static void
on_error(GstElement *branch, const char *msg)
{
    for (int i = 0; i < 2; i++) {
        Track *t = &tracks[i];
        if (t->branch != branch)
            continue;
        errors_reported++;
        t->failed = TRUE;
        t->branch = t->vol = t->amp = NULL;
        t->vol_cs = t->amp_cs = NULL;
        t->mixpad             = NULL;
    }
}

static gpointer
watchdog(gpointer unused)
{
    g_usleep(5 * G_USEC_PER_SEC);
    if (!g_atomic_int_get(&survived)) {
        fprintf(stderr, "FAIL: main loop did not survive branch teardown (deadlock)\n");
        _exit(1);
    }
    return NULL;
}

static gboolean
check_playing(gpointer unused)
{
    gint64 pos = player_position();
    assert(pos > pos_before && "the surviving track still plays");
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static gboolean
after_teardown(gpointer unused)
{
    g_atomic_int_set(&survived, TRUE);
    assert(errors_reported == 1 && "the error was reported once");
    assert(!tracks[0].branch && tracks[1].branch);
    assert(player_play());
    pos_before = player_position();
    g_timeout_add(1000, check_playing, NULL);
    return G_SOURCE_REMOVE;
}

static void
find_decoder(const GValue *v, gpointer out)
{
    GstElement *e = g_value_get_object(v);
    if (g_str_has_prefix(GST_OBJECT_NAME(e), "uridecodebin"))
        *(GstElement **)out = e;
}

// From the main thread -- not the parked streaming thread -- post an error as
// the first track's decoder, as a demuxer thread reading ahead would.
static gboolean
inject(gpointer unused)
{
    GstElement  *dec = NULL;
    GstIterator *it  = gst_bin_iterate_elements(GST_BIN(tracks[0].branch));
    gst_iterator_foreach(it, find_decoder, &dec);
    gst_iterator_free(it);
    assert(dec);
    GError *err = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_DECODE, "injected");
    gst_element_post_message(dec, gst_message_new_error(GST_OBJECT(dec), err, "injected"));
    g_error_free(err);
    // Runs only if the bus watch (and release_branch) returned.
    g_timeout_add(300, after_teardown, NULL);
    return G_SOURCE_REMOVE;
}

static char *
make_wav(const char *dir, const char *name, int seconds)
{
    char       *path = g_build_filename(dir, name, NULL);
    char       *desc = g_strdup_printf("audiotestsrc samplesperbuffer=4800 num-buffers=%d ! "
                                             "audio/x-raw,rate=48000,channels=2 ! wavenc ! filesink name=out",
                                       seconds * 10);
    GstElement *gen  = gst_parse_launch(desc, NULL);
    g_free(desc);
    GstElement *out = gst_bin_get_by_name(GST_BIN(gen), "out");
    g_object_set(out, "location", path, NULL);
    gst_object_unref(out);
    gst_element_set_state(gen, GST_STATE_PLAYING);
    GstBus     *bus = gst_element_get_bus(gen);
    GstMessage *msg
        = gst_bus_timed_pop_filtered(bus, 30 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    assert(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS);
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(gen, GST_STATE_NULL);
    gst_object_unref(gen);
    return path;
}

int
main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    char *dir = g_dir_make_tmp("ac-teardown-XXXXXX", NULL);
    char *a   = make_wav(dir, "a.wav", 30);
    char *b   = make_wav(dir, "b.wav", 30);

    loop = g_main_loop_new(NULL, FALSE);
    player_set_error_handler(on_error);
    player_init();
    for (int i = 0; i < 2; i++) {
        tracks[i].uri = g_filename_to_uri(i ? b : a, NULL, NULL);
        player_add(&tracks[i]);
    }
    player_wait_ready();
    if (player_position() < 0) {
        fprintf(stderr, "SKIP: pipeline did not preroll (no audio sink?)\n");
        return 77;
    }

    // Give the decoders time to fill the mixer's pad queues and park.
    g_timeout_add(500, inject, NULL);
    g_thread_unref(g_thread_new("watchdog", watchdog, NULL));
    g_main_loop_run(loop);

    player_shutdown();
    g_unlink(a);
    g_unlink(b);
    g_rmdir(dir);
    g_print("teardown checks passed\n");
    return 0;
}
