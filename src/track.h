#pragma once

#include <gst/gst.h>

typedef struct {
    float min, max;
} Peak;

typedef struct {
    char       *uri;
    char       *name;     // basename, for display
    GstElement *pipeline;  // playbin
    guint       bus_watch; // bus watch source id
    GArray     *peaks;     // of Peak, one per SAMPLES_PER_PEAK samples
    gint64      duration;  // ns, -1 if unknown
    double      lufs;      // integrated loudness, -HUGE_VAL if silent
    double      dbtp;      // max true peak in dBTP, -HUGE_VAL if silent
} Track;

Track *track_new(const char *uri);
void   track_free(Track *t);

void   track_play(Track *t);
void   track_pause(Track *t);
gint64 track_position(Track *t); // ns, -1 if unknown
void   track_seek(Track *t, gint64 pos);
