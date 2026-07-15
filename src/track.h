#pragma once

#include <gst/gst.h>

typedef struct {
    float min, max;
} Peak;

typedef struct {
    char    *uri;
    char    *name;     // basename, for display
    GArray  *peaks;    // of Peak, one per SAMPLES_PER_PEAK samples
    gint64   duration; // ns, -1 if unknown
    double   lufs;     // integrated loudness, -HUGE_VAL if silent
    double   dbtp;     // max true peak in dBTP, -HUGE_VAL if silent
    int      bus;      // bus id 0-9, or -1 for none
    gboolean inverted; // polarity flipped (x-1)
    gboolean muted;    // silenced regardless of the audible set
    gboolean soloed;   // when any track in the bus is soloed, only solos play

    // Player branch handles (owned by the player, see player.c).
    GstElement       *branch; // bin: uridecodebin->convert->resample->amplify->volume
    GstElement       *vol;    // volume element inside the branch, toggled to mute
    GstElement       *amp;    // audioamplify, +1 normal / -1 inverted
    GstControlSource *amp_cs; // drives amp polarity, ramped to declick
    GstPad           *mixpad; // requested audiomixer sink pad
} Track;

Track *track_new(const char *uri);
void   track_free(Track *t);
