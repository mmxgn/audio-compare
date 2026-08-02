#pragma once

#include "track.h"

// Single audiomixer pipeline shared by all tracks. Every track feeds the mixer
// through its own muted-by-default volume branch, so the audible set plays
// together in sample-accurate sync (one clock).
//
// Switching the audible set is done by player_set_audible(), which applies the
// change via a GstControlSource at the current running time so all tracks flip
// at exactly the same audio sample. For identical content this is seamless;
// for different content the irreducible one-sample discontinuity is the accepted
// trade-off of A/B comparison.

void player_init(void);
void player_shutdown(void);

void player_add(Track *t);    // build and attach the track's audio branch
void player_remove(Track *t); // detach and tear down the track's branch

// Mute or unmute a track, sample-accurately at pipeline running time `when`.
// Pass the same `when` for every track in a single apply_audible() sweep so
// all transitions land on the same audio sample.
void player_set_audible(Track *t, gboolean audible, GstClockTime when);

void player_set_inverted(Track *t, gboolean inverted); // polarity x-1

void   player_play(void);
void   player_pause(void);
void   player_seek(gint64 pos);
gint64 player_position(void); // ns, -1 if unknown
gint64 player_running_time(void); // ns running time for passing to player_set_audible
