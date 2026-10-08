// Measurement harness for player_set_audible(). Builds the same pipeline the
// app does, feeds it two sine files of known amplitude and probes each branch's
// output so the gain the control source actually applied can be read back per
// buffer. Build with -DOLD to measure the pre-fix behaviour.
//
//   ./harness <scenario> <a.wav> <b.wav>
#include "player.h"
#include "track.h"

#include <gst/controller/gsttimedvaluecontrolsource.h>
#include <gst/gst.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

#define NEV 40000
#define TOL 0.10 // gain tolerance when classifying a buffer

typedef struct {
    gint64 pts, dur;
    double head; // gain over the first ~10 ms of the buffer
    double whole;
} Ev;

static Ev           evs[2][NEV];
static int          nev[2];
static const double nominal[2] = { 0.05, 0.025 }; // source amplitudes
static Track        tr[2];
static gint64       last_pts[2], frontier;
static GstClockTime switch_at;  // stream time the switch was scheduled for
static gint64       switch_pos; // where playback was when it was requested
static double       sw_from[2], sw_to[2];
static int          failures;

static double
peak(const float *d, guint n)
{
    double m = 0;
    for (guint i = 0; i < n; i++)
        if (fabs(d[i]) > m)
            m = fabs(d[i]);
    return m;
}

static GstPadProbeReturn
probe(GstPad *pad, GstPadProbeInfo *info, gpointer user)
{
    int        k   = GPOINTER_TO_INT(user);
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    GstMapInfo map;
    if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
        const float *d = (const float *)map.data;
        guint        n = map.size / sizeof(float);
        guint        q = n / 4 ? n / 4 : n;
        if (nev[k] < NEV) {
            Ev *e    = &evs[k][nev[k]++];
            e->pts   = GST_BUFFER_PTS(buf);
            e->dur   = GST_BUFFER_DURATION(buf);
            e->head  = peak(d, q) / nominal[k];
            e->whole = peak(d, n) / nominal[k];
        }
        last_pts[k] = GST_BUFFER_PTS(buf);
        gst_buffer_unmap(buf, &map);
    }
    return GST_PAD_PROBE_OK;
}

static GMainLoop *loop;
static gpointer
loopthread(gpointer u)
{
    g_main_loop_run(loop);
    return NULL;
}

static void
sleep_ms(int ms)
{
    // Track how far past the rendering position the branch output has already
    // run: that is what the switch point has to clear.
    for (int i = 0; i < ms; i += 10) {
        gint64 f = last_pts[0] + 40 * GST_MSECOND - player_position();
        if (f > frontier) {
            frontier = f;
            if (g_getenv("TRACE_FRONTIER"))
                g_print("   frontier %.3f (pos %.3f lastpts %.3f)\n", (double)f / GST_SECOND,
                        (double)player_position() / GST_SECOND, (double)last_pts[0] / GST_SECOND);
        }
        g_usleep(10000);
    }
}

static int
pointcount(int k)
{
    return (int)gst_timed_value_control_source_get_count(
        GST_TIMED_VALUE_CONTROL_SOURCE(tr[k].vol_cs));
}

// The call site under test, mirroring main.c:apply_audible().
static void
apply_audible(gboolean a_on, gboolean b_on)
{
#ifdef OLD
    GstClockTime when = (GstClockTime)player_running_time();
#else
    GstClockTime when = player_switch_time();
#endif
    gboolean on[2] = { a_on, b_on };
    for (int k = 0; k < 2; k++) {
        sw_from[k] = nev[k] ? evs[k][nev[k] - 1].head : 0.0;
        sw_to[k]   = on[k] ? 1.0 : 0.0;
        player_set_audible(&tr[k], on[k], when);
    }
    switch_at  = when;
    switch_pos = player_position();
    g_print("  switch: when=%.3f pos=%.3f points=(%d,%d)\n", (double)when / GST_SECOND,
            (double)switch_pos / GST_SECOND, pointcount(0), pointcount(1));
}

static void
fail(const char *why, ...)
{
    va_list ap;
    va_start(ap, why);
    char *m = g_strdup_vprintf(why, ap);
    va_end(ap);
    g_print("  FAIL %s\n", m);
    g_free(m);
    failures++;
}

// The gain each branch is playing at right now.
static void
check_state(const char *what, double a_want, double b_want)
{
    double a = nev[0] ? evs[0][nev[0] - 1].head : -1;
    double b = nev[1] ? evs[1][nev[1] - 1].head : -1;
    g_print("  %s: branch0=%.2f (want %.0f) branch1=%.2f (want %.0f)\n", what, a, a_want, b,
            b_want);
    if (fabs(a - a_want) > TOL || fabs(b - b_want) > TOL)
        fail("%s: audible set is (%.2f,%.2f), wanted (%.0f,%.0f)", what, a, b, a_want, b_want);
}

// The audible set applied before the first play has to be in force once audio
// starts. An emptied control source shows up here as a gain stuck at whatever
// the binding last synced.
static void
check_initial(void)
{
    check_state("initial state", 1.0, 0.0);
}

// Classify every buffer around the switch point: how many sit at an in-between
// gain (a fade rather than a step), when the target gain was first reached, and
// whether the change leaked into buffers that were already in flight.
static void
check_switch(const char *scen)
{
    double land[2] = { -1, -1 };
    for (int k = 0; k < 2; k++) {
        if (fabs(sw_to[k] - sw_from[k]) < TOL)
            continue; // this branch was not asked to change
        // Look from just before the keypress to well after the switch point --
        // far enough back to catch a change leaking into audio that was already
        // in flight, not so far back that an earlier switch is swept in.
        int    middling = 0, early = 0;
        gint64 lo = switch_pos > 200 * GST_MSECOND ? switch_pos - 200 * GST_MSECOND : 0;
        for (int i = 0; i < nev[k]; i++) {
            Ev *e = &evs[k][i];
            if (e->pts < lo || e->pts > (gint64)switch_at + 1500 * GST_MSECOND)
                continue;
            gboolean at_from = fabs(e->head - sw_from[k]) < TOL;
            gboolean at_to   = fabs(e->head - sw_to[k]) < TOL;
            if (!at_from && !at_to)
                middling++;
            if (!at_from && e->pts + 2 * e->dur < (gint64)switch_at)
                early++;
            if (at_to && land[k] < 0 && e->pts + e->dur > (gint64)switch_at)
                land[k] = (double)e->pts;
        }
        double tail = -1; // steady state well after the switch
        for (int i = nev[k] - 1; i >= 0; i--)
            if (evs[k][i].pts > (gint64)switch_at + 500 * GST_MSECOND) {
                tail = evs[k][i].head;
                break;
            }
        g_print("  branch%d %.0f->%.0f: land=%+.0f ms vs scheduled (%+.0f ms vs keypress)  "
                "fading buffers=%d  early=%d  settled=%.2f\n",
                k, sw_from[k], sw_to[k], land[k] < 0 ? 9999.0 : (land[k] - (double)switch_at) / 1e6,
                land[k] < 0 ? 9999.0 : (land[k] - (double)switch_pos) / 1e6, middling, early, tail);
        if (land[k] < 0)
            fail("%s: branch%d never reached %.0f (no-op)", scen, k, sw_to[k]);
        else if (fabs(land[k] - (double)switch_at) > 100 * GST_MSECOND)
            fail("%s: branch%d landed %+.0f ms off", scen, k, (land[k] - (double)switch_at) / 1e6);
        if (middling > 1)
            fail("%s: branch%d faded over %d buffers instead of stepping", scen, k, middling);
        if (early)
            fail("%s: branch%d changed %d buffers before the switch point", scen, k, early);
        if (tail >= 0 && fabs(tail - sw_to[k]) > TOL)
            fail("%s: branch%d settled at %.2f, wanted %.0f", scen, k, tail, sw_to[k]);
    }
    if (land[0] >= 0 && land[1] >= 0 && fabs(land[0] - land[1]) > 50 * GST_MSECOND)
        fail("%s: branches crossed %.0f ms apart", scen, fabs(land[0] - land[1]) / 1e6);
}

static void
dump(gint64 from, gint64 to)
{
    for (int k = 0; k < 2; k++) {
        g_print("   branch%d:", k);
        int printed = 0;
        for (int i = 0; i < nev[k] && printed < 20; i++)
            if (evs[k][i].pts >= from && evs[k][i].pts <= to) {
                g_print(" %.2f=%.2f", (double)evs[k][i].pts / GST_SECOND, evs[k][i].head);
                printed++;
            }
        g_print("\n");
    }
}

int
main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    const char *scen = argc > 1 ? argv[1] : "baseline";

    loop = g_main_loop_new(NULL, FALSE);
    player_init();
    tr[0].uri = gst_filename_to_uri(argv[2], NULL);
    tr[1].uri = gst_filename_to_uri(argv[3], NULL);
    tr[0].bus = tr[1].bus = -1;
    player_add(&tr[0]);
    player_add(&tr[1]);
    for (int k = 0; k < 2; k++) {
        GstPad *p = gst_element_get_static_pad(tr[k].branch, "src");
        gst_pad_add_probe(p, GST_PAD_PROBE_TYPE_BUFFER, probe, GINT_TO_POINTER(k), NULL);
        gst_object_unref(p);
    }
    g_thread_new("loop", loopthread, NULL);
    GstObject *pipe = gst_element_get_parent(tr[0].branch);
    gst_element_get_state(GST_ELEMENT(pipe), NULL, NULL, 5 * GST_SECOND);
    gst_object_unref(pipe);

    g_print("== %s ==\n", scen);

    if (!strcmp(scen, "premute")) {
        // Mute A / unmute B before the pipeline has ever played.
        apply_audible(FALSE, TRUE);
        player_play();
        sleep_ms(1500);
        g_print("  first 400 ms:\n");
        dump(0, 400 * GST_MSECOND);
        for (int i = 0; i < nev[0]; i++)
            if (evs[0][i].whole > TOL) {
                fail("premute: muted branch0 audible (%.2f) at %.3f s", evs[0][i].whole,
                     (double)evs[0][i].pts / GST_SECOND);
                break;
            }
        gint64 up = -1;
        for (int i = 0; i < nev[1]; i++)
            if (evs[1][i].head > 1.0 - TOL) {
                up = evs[1][i].pts;
                break;
            }
        g_print("  branch1 reached unity at %.3f s\n", (double)up / GST_SECOND);
        if (up < 0 || up > 150 * GST_MSECOND)
            fail("premute: unmuted branch1 only audible at %.3f s", (double)up / GST_SECOND);
    } else if (!strcmp(scen, "soak")) {
        // The in-flight frontier deepens with every pause/resume before it
        // settles; check it still fits inside the lead player.h assumes.
        apply_audible(TRUE, FALSE);
        player_play();
        for (int i = 0; i < 6; i++) {
            sleep_ms(1500);
            player_pause();
            sleep_ms(700);
            player_play();
            g_print("   after pause %d: frontier %.3f s\n", i + 1, (double)frontier / GST_SECOND);
        }
        sleep_ms(1000);
        if (frontier > (gint64)SWITCH_LEAD)
            fail("soak: frontier %.3f s exceeds SWITCH_LEAD %.3f s", (double)frontier / GST_SECOND,
                 (double)SWITCH_LEAD / GST_SECOND);
    } else if (!strcmp(scen, "count")) {
        apply_audible(TRUE, FALSE);
        player_play();
        sleep_ms(700);
        check_initial();
        for (int i = 0; i < 10; i++) {
            sleep_ms(500);
            apply_audible(i % 2, !(i % 2));
        }
        sleep_ms(1000);
        g_print("  final points=(%d,%d), last commanded set is (1,0)\n", pointcount(0),
                pointcount(1));
        if (pointcount(0) > 8 || pointcount(1) > 8)
            fail("count: control points grew to (%d,%d)", pointcount(0), pointcount(1));

        // Replay the stretch the switches happened in. Points left lying around
        // there make it a linear crossfade of the whole switching history
        // instead of the set that was last commanded.
        player_seek(1500 * GST_MSECOND);
        sleep_ms(600);
        nev[0] = nev[1] = 0;
        sleep_ms(3000);
        int mid = 0;
        for (int k = 0; k < 2; k++)
            for (int i = 0; i < nev[k]; i++)
                if (evs[k][i].head > TOL && evs[k][i].head < 1.0 - TOL)
                    mid++;
        g_print("  replaying it: %d buffers at an in-between gain\n", mid);
        g_print("  gains on the replay (pts=gain):\n");
        dump(1500 * GST_MSECOND, 4500 * GST_MSECOND);
        check_state("replaying the switched stretch", 1.0, 0.0);
        if (mid > 25) // the one bounded in-flight window, if the replay crosses it
            fail("count: %d buffers mid-crossfade, switches are fading not stepping", mid);
    } else {
        gboolean resume = FALSE;
        apply_audible(TRUE, FALSE); // A audible to start with
        player_play();
        sleep_ms(700);
        check_initial();
        if (!strcmp(scen, "baseline")) {
            // One warm-up switch first: the very first switch of a session is
            // not representative, the control source is still in its as-built
            // state.
            sleep_ms(800);
            apply_audible(FALSE, TRUE);
            sleep_ms(1200);
            apply_audible(TRUE, FALSE);
            sleep_ms(1200);
        } else if (!strcmp(scen, "pause")) {
            sleep_ms(800);
            apply_audible(FALSE, TRUE);
            sleep_ms(1000);
            apply_audible(TRUE, FALSE);
            sleep_ms(1000);
            // Leave it paused: the switch under test is made during the pause,
            // which is what sends running time and stream time furthest apart.
            player_pause();
            sleep_ms(3000);
            resume = TRUE;
        } else if (!strcmp(scen, "seek")) {
            // Switch, then seek back behind the switch point: the audible set
            // has to survive, not revert to what it was before the switch.
            sleep_ms(1500);
            apply_audible(FALSE, TRUE);
            gint64 lo = switch_pos, hi = switch_at;
            sleep_ms(1500);
            player_seek(300 * GST_MSECOND);
            sleep_ms(600);
            check_state("right after a backward seek", 0.0, 1.0);
            nev[0] = nev[1] = 0; // only count the replay from here on
            sleep_ms(2500);      // replay across the stretch that was in flight
            check_state("after replaying past the switch point", 0.0, 1.0);
            // That stretch was already committed to the sink when the switch
            // was made, so replaying it replays the old mix. Bounded by the
            // lead; anything wider means the guard region is misplaced.
            gint64 stale = 0;
            for (int i = 0; i < nev[0]; i++)
                if (evs[0][i].pts >= lo && evs[0][i].pts <= hi && evs[0][i].head > 1.0 - TOL)
                    stale += evs[0][i].dur;
            g_print("  old mix replayed for %.3f s around the switch point\n",
                    (double)stale / GST_SECOND);
            if (stale > (gint64)SWITCH_LEAD + 100 * GST_MSECOND)
                fail("seek: %.3f s of stale mix on replay", (double)stale / GST_SECOND);
            apply_audible(TRUE, FALSE); // put A back so the test switch is A->B
            sleep_ms(1500);
        } else if (!strcmp(scen, "eos")) {
            // Same, through the EOS loop back to 0.
            sleep_ms(500);
            apply_audible(FALSE, TRUE);
            sleep_ms(1500);
            player_seek(9 * GST_SECOND + 500 * GST_MSECOND);
            sleep_ms(2500); // runs off the end and the bus handler loops it
            check_state("after the EOS loop", 0.0, 1.0);
            nev[0] = nev[1] = 0; // PTS restarts at 0, drop the first pass
            apply_audible(TRUE, FALSE);
            sleep_ms(1500);
        }
        apply_audible(FALSE, TRUE); // the switch under test
        if (resume)
            player_play();
        sleep_ms(3000);
        check_switch(scen);
        g_print("  gains from the moment of the switch (pts=gain):\n");
        dump(switch_pos - 200 * GST_MSECOND, switch_pos + 2500 * GST_MSECOND);
    }

    g_print("  worst in-flight frontier seen: %.3f s\n", (double)frontier / GST_SECOND);
    g_print("RESULT %s %s\n", scen, failures ? "FAIL" : "PASS");
    player_shutdown();
    return failures ? 1 : 0;
}
