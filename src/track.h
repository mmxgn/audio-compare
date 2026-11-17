#pragma once

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

typedef struct {
    float min, max;
} Peak;

typedef struct {
    char    *uri;
    char    *name;      // basename, for display
    GArray  *peaks;     // of Peak, one per SAMPLES_PER_PEAK samples
    gint64   duration;  // ns, -1 if unknown
    double   lufs;      // integrated loudness, -HUGE_VAL if silent
    double   dbtp;      // max true peak in dBTP, -HUGE_VAL if silent
    int      bus;       // bus id 0-9, or -1 for none
    gboolean inverted;  // polarity flipped (x-1)
    gboolean muted;     // silenced regardless of the audible set
    gboolean soloed;    // when any track in the bus is soloed, only solos play
    gboolean failed;    // its branch errored: not re-added on engine reset
    gboolean truncated; // analysis hit the length cap: peaks cover only the start
    gboolean stalled;   // analysis gave up on a source that stopped producing

    // Player branch handles (owned by the player, see player.c).
    GstElement       *branch; // bin: uridecodebin->convert->resample->amplify->volume
    GstElement       *vol;    // volume element inside the branch
    GstElement       *amp;    // audioamplify, +POLARITY normal / -POLARITY inverted
    GstControlSource *vol_cs; // drives vol, switched sample-accurately on track change
    GstControlSource *amp_cs; // drives amp polarity, ramped to declick
    GstPad           *mixpad; // requested audiomixer sink pad
} Track;

Track *track_new(const char *uri);
// The fixed analysis chain with `uri` set as a property; NULL on failure.
GstElement *track_analysis_pipeline(const char *uri, GstAppSink **sink_out);
// Analyse a running pipeline's appsink into t, within the given limits.
void track_analyze(Track *t, GstElement *pipe, GstAppSink *sink, guint64 max_frames,
                   GstClockTime stall_timeout);
void track_free(Track *t);
