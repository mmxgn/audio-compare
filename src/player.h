#pragma once

#include "track.h"

// Single audiomixer pipeline shared by all tracks. Every track feeds the mixer
// through its own muted-by-default volume branch, so the audible set plays
// together in sample-accurate sync (one clock).
//
// Switching the audible set is done by player_set_audible(), which schedules
// the change on a GstControlSource at a stream time ahead of the buffers
// already in flight. All tracks given the same `when` cross on the same sample,
// over a 5 ms declick ramp. The cost is latency: the switch lands SWITCH_LEAD
// after the keypress. Before the pipeline has ever played there is nothing in
// flight and the change lands at once.

void player_init(void);
void player_shutdown(void);

void player_add(Track *t);    // build and attach the track's audio branch
void player_remove(Track *t); // detach and tear down the track's branch

// How far ahead of the audible position a switch is scheduled. It has to clear
// every buffer already pushed past `volume`, which is the audio sink's ring
// buffer (200 ms by default) plus the mixer's and the decoders' own queueing.
//
// ponytail: measured, not queried. GST_QUERY_LATENCY reports min=0/max=NONE on
// this non-live pipeline, and the exact figure -- a position query upstream of
// each audiomixer sink pad -- is wrong for a branch that has hit EOS, which
// reports its whole duration. test/harness.c measures the real frontier: 0.31 s
// playing straight through, settling at 0.38 s after a few pause/resume cycles,
// so 0.45 s with headroom. verify.sh re-checks that ceiling. Overshooting it
// only costs alignment: the switch then lands at the next buffer boundary.
#define SWITCH_LEAD (450 * GST_MSECOND)

// Mute or unmute a track at stream time `when`. Pass one player_switch_time()
// to every track in a single apply_audible() sweep so all of them cross on the
// same audio sample.
void         player_set_audible(Track *t, gboolean audible, GstClockTime when);
GstClockTime player_switch_time(void); // stream time to schedule that sweep at

void player_set_inverted(Track *t, gboolean inverted); // polarity x-1

void   player_play(void);
void   player_pause(void);
void   player_seek(gint64 pos);
gint64 player_position(void);     // ns, -1 if unknown
gint64 player_running_time(void); // ns pipeline running time, used by player_set_inverted
