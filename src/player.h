#pragma once

#include "track.h"

// Single audiomixer pipeline shared by all tracks. Every track feeds the mixer
// through its own muted-by-default volume branch, so the audible set plays
// together in sample-accurate sync (one clock) and switching is instant.

void player_init(void);
void player_shutdown(void);

void player_add(Track *t);    // build and attach the track's audio branch
void player_remove(Track *t); // detach and tear down the track's branch

void player_set_audible(Track *t, gboolean audible);   // unmute / mute
void player_set_inverted(Track *t, gboolean inverted); // polarity x-1

void   player_play(void);
void   player_pause(void);
void   player_seek(gint64 pos);
gint64 player_position(void); // ns, -1 if unknown
