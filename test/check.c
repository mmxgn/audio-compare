// Assert-based checks for the pure-logic UI/state fixes (bugs 1, 6, 7, 8).
// The sources are included so the real static functions can be driven directly;
// the player is stubbed out so no pipeline, window or display is needed.
#include <adwaita.h>
#include <assert.h>
#include <glib/gstdio.h>

static int redraws;
#define gtk_widget_queue_draw(w) (redraws++)

#include "../src/waveform.c"

#define main real_main
#include "../src/main.c"
#undef main

// ---- player stubs: record what the code under test asked for ----------------
static Track    tracks[8];
static gint64   stub_pos;
static gint64   last_seek;
static int      seeks;
static gboolean stub_audible[8];

void
player_set_audible(Track *t, gboolean audible, GstClockTime when)
{
    for (int i = 0; i < 8; i++)
        if (t == &tracks[i])
            stub_audible[i] = audible;
}
void
player_seek(gint64 pos)
{
    last_seek = pos;
    seeks++;
}
gint64
player_position(void)
{
    return stub_pos;
}
gint64
player_running_time(void)
{
    return 0;
}
void
player_init(void)
{
}
void
player_shutdown(void)
{
}
void
player_add(Track *t)
{
}
void
player_remove(Track *t)
{
}
void
player_set_inverted(Track *t, gboolean inverted)
{
}
gboolean
player_play(void)
{
    return TRUE;
}
void
player_set_error_handler(PlayerErrorFn fn)
{
}
GstClockTime
player_switch_time(void)
{
    return 0;
}
void
player_wait_ready(void)
{
}
void
player_pause(void)
{
}
Track *
track_new(const char *uri)
{
    return NULL;
}
void
track_free(Track *t)
{
}
void
track_analysis_start(const char *uri, GCancellable *cancel, GAsyncReadyCallback cb, gpointer user)
{
}
// What the next track_analysis_finish() hands back: a result, or (NULL) the
// cancel GTask reports once the Track was freed.
static TrackAnalysis *stub_result;
TrackAnalysis *
track_analysis_finish(GAsyncResult *res, GError **err)
{
    if (!stub_result)
        g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
    TrackAnalysis *a = stub_result;
    stub_result      = NULL;
    return a;
}
void
track_analysis_free(TrackAnalysis *a)
{
    g_array_free(a->peaks, TRUE);
    g_free(a);
}

// ---- harness ---------------------------------------------------------------

// A stand-in for the drawing area: waveform.c only ever reaches its state
// through g_object_get_data(), so a plain GObject is enough.
static GtkWidget *
fake_wave(Track *t)
{
    GObject *o = g_object_new(G_TYPE_OBJECT, NULL);
    WfData  *d = g_new0(WfData, 1);
    d->track   = t;
    g_object_set_data_full(o, "wf", d, g_free);
    return (GtkWidget *)o;
}

static void
reset(int n)
{
    app.tracks = g_ptr_array_new();
    app.waves  = g_ptr_array_new();
    app.rows   = g_ptr_array_new();
    app.active = -1;
    seeks = redraws = 0;
    memset(stub_audible, 0, sizeof stub_audible);
    for (int i = 0; i < n; i++) {
        memset(&tracks[i], 0, sizeof tracks[i]);
        tracks[i].bus      = -1;
        tracks[i].duration = -1;
        tracks[i].name     = "t";
        tracks[i].peaks    = g_array_new(FALSE, FALSE, sizeof(Peak));
        g_ptr_array_add(app.tracks, &tracks[i]);
        g_ptr_array_add(app.waves, fake_wave(&tracks[i]));
    }
}

static double
playhead_of(int i)
{
    WfData *d = g_object_get_data(G_OBJECT(g_ptr_array_index(app.waves, i)), "wf");
    return d->playhead;
}

// What apply_audible() actually told the player for track i.
static gboolean
plays(int i)
{
    return stub_audible[i];
}

// Bug 1: soloing a muted track must not silence its whole bus.
static void
test_solo_on_muted_track(void)
{
    reset(2);
    tracks[0].bus = tracks[1].bus = 1;
    tracks[0].muted = tracks[0].soloed = TRUE;
    app.active                         = 0;
    apply_audible();
    assert(!plays(0) && "a muted track never plays");
    assert(plays(1) && "soloing a muted track must not silence the bus");

    // a normal solo still suppresses the rest of the bus
    reset(2);
    tracks[0].bus = tracks[1].bus = 1;
    tracks[0].soloed              = TRUE;
    app.active                    = 0;
    apply_audible();
    assert(plays(0) && !plays(1) && "solo still scopes the bus to the soloed track");
}

// Bug 6: a shared position past a shorter track's duration must not push the
// playhead outside the pane.
static void
test_playhead_clamped(void)
{
    reset(2);
    tracks[0].duration = 10 * GST_SECOND;
    tracks[1].duration = 3 * GST_SECOND;
    app.active         = 0;
    stub_pos           = 5 * GST_SECOND;
    tick(NULL);
    assert(playhead_of(0) > 0.49 && playhead_of(0) < 0.51);
    assert(playhead_of(1) <= 1.0 && "playhead must stay inside the pane");
    assert(playhead_of(1) == 1.0 && "an ended track parks at the end");
}

// Bug 7: clicking a track with unknown duration must not seek to a negative
// position (gst_element_seek_simple asserts seek_pos >= 0).
static void
test_no_seek_without_duration(void)
{
    reset(1);
    app.active         = 0;
    tracks[0].duration = -1; // header-only file: zero frames, no duration
    on_wave_click(g_ptr_array_index(app.waves, 0), 1.0, NULL);
    tick(NULL);
    assert(seeks == 0 && "no seek at all when the duration is unknown");

    tracks[0].duration = 4 * GST_SECOND;
    on_wave_click(g_ptr_array_index(app.waves, 0), 1.0, NULL);
    tick(NULL); // seeks are coalesced onto the tick, see test_scrub_seeks_coalesce
    assert(seeks == 1 && last_seek == 4 * GST_SECOND && "a known duration still seeks");
}

// A drag emits one motion event per frame; each used to seek inline, and every
// flushing seek repositions every branch. Only the last target per frame matters.
static void
test_scrub_seeks_coalesce(void)
{
    reset(1);
    app.active         = 0;
    tracks[0].duration = 10 * GST_SECOND;
    GtkWidget *wf      = g_ptr_array_index(app.waves, 0);

    on_wave_scrub(wf, TRUE, NULL);
    for (int i = 1; i <= 50; i++) // a 50-event drag inside one frame
        on_wave_click(wf, i / 100.0, NULL);
    assert(seeks == 0 && "a drag must not seek inline");

    tick(NULL);
    assert(seeks == 1 && "one seek per frame, however many motion events");
    assert(last_seek == 5 * GST_SECOND && "the surviving seek is the newest target");

    for (int i = 0; i < 10; i++) // nothing new: no further seeks
        tick(NULL);
    assert(seeks == 1 && "an idle tick must not re-seek");

    on_wave_click(wf, 0.8, NULL); // drag-end must land before playback resumes
    on_wave_scrub(wf, FALSE, NULL);
    assert(seeks == 2 && last_seek == 8 * GST_SECOND && "drag-end flushes the last target");
}

// Bug 8: the 33 ms tick must not redraw a pane whose playhead did not move.
static void
test_playhead_redraw_is_guarded(void)
{
    reset(1);
    GtkWidget *wf = g_ptr_array_index(app.waves, 0);
    waveform_set_playhead(wf, 0.25);
    assert(redraws == 1);
    waveform_set_playhead(wf, 0.25);
    waveform_set_playhead(wf, 0.25);
    assert(redraws == 1 && "an unchanged playhead must not queue a redraw");
    waveform_set_playhead(wf, 0.26);
    assert(redraws == 2 && "a moved playhead still redraws");

    // the same, driven through the real tick() while paused
    reset(1);
    tracks[0].duration = 10 * GST_SECOND;
    app.active         = 0;
    stub_pos           = 2 * GST_SECOND;
    tick(NULL);
    int after_first = redraws;
    for (int i = 0; i < 30; i++)
        tick(NULL);
    assert(redraws == after_first && "a paused tick must not redraw every frame");
}

// Only regular local files become tracks, and their URI is always the escaped
// form GLib builds -- never the raw string a placeholder GFile carries.
static void
test_track_uri_gate(void)
{
    char *dir = g_dir_make_tmp("ac-uri-XXXXXX", NULL);
    assert(dir);
    char *path = g_build_filename(dir, "a\" ! identity name=canary ! b.wav", NULL);
    assert(g_file_set_contents(path, "", 0, NULL));

    GFile *f   = g_file_new_for_path(path);
    char  *uri = local_audio_uri(f);
    assert(uri && "a regular local file is accepted");
    assert(!strchr(uri, '"') && !strchr(uri, ' ') && "its URI is percent-encoded");
    g_free(uri);
    g_object_unref(f);

    f = g_file_new_for_path(dir);
    assert(!local_audio_uri(f) && "a directory is rejected");
    g_object_unref(f);

    f = g_file_new_for_uri("http://example.invalid/x.flac");
    assert(!local_audio_uri(f) && "a remote URI is rejected");
    g_object_unref(f);

    f = g_file_new_for_commandline_arg("foo:\" ! identity name=canary ! fakesink name=\"b");
    assert(!local_audio_uri(f) && "a placeholder scheme: argument is rejected");
    g_object_unref(f);

    g_unlink(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
}

// The worker's completion moves its result into the Track and redraws it, but
// a completion for a cancelled analysis (pane closed, Track freed) must not
// touch the Track at all.
static void
test_analysis_completion(void)
{
    reset(1);
    app.active          = 0;
    tracks[0].analysing = TRUE;
    app.analysing       = 2;

    // cancelled: the user pointer is stale; a poisoned one would crash if used
    stub_result = NULL;
    on_analysed(NULL, NULL, (gpointer)0x1);
    assert(app.analysing == 1 && "a cancelled completion still counts as done");
    assert(redraws == 0);

    TrackAnalysis *a = g_new0(TrackAnalysis, 1);
    a->peaks         = g_array_new(FALSE, FALSE, sizeof(Peak));
    Peak p           = { -0.5f, 0.5f };
    g_array_append_val(a->peaks, p);
    a->duration = 3 * GST_SECOND;
    a->lufs     = -14.0;
    a->dbtp     = -1.0;
    stub_result = a;
    on_analysed(NULL, NULL, &tracks[0]);
    assert(app.analysing == 0);
    assert(!tracks[0].analysing && tracks[0].peaks->len == 1);
    assert(tracks[0].duration == 3 * GST_SECOND && tracks[0].lufs == -14.0);
    assert(redraws == 1 && "the pane is redrawn with the new peaks");
}

// Optional argument selects one bug's check, so each can be shown to fail on
// its own against the unfixed code; no argument runs them all.
int
main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : NULL;
    if (!only || g_str_equal(only, "1"))
        test_solo_on_muted_track();
    if (!only || g_str_equal(only, "6"))
        test_playhead_clamped();
    if (!only || g_str_equal(only, "7"))
        test_no_seek_without_duration();
    if (!only || g_str_equal(only, "8"))
        test_playhead_redraw_is_guarded();
    if (!only || g_str_equal(only, "9"))
        test_scrub_seeks_coalesce();
    if (!only || g_str_equal(only, "uri"))
        test_track_uri_gate();
    if (!only || g_str_equal(only, "async"))
        test_analysis_completion();
    g_print("checks passed: %s\n", only ? only : "1 6 7 8 9 uri async");
    return 0;
}
