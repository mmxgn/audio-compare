#pragma once

#include <gio/gio.h>
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
    gboolean analysing; // analysis still running: peaks/duration/loudness not yet known

    // Cancels the in-flight analysis. track_free() cancels it, so a completion
    // that arrives after the Track is gone sees G_IO_ERROR_CANCELLED and must
    // not touch the Track again.
    GCancellable *cancel;

    // Player branch handles (owned by the player, see player.c).
    GstElement       *branch; // bin: uridecodebin->convert->resample->amplify->volume
    GstElement       *vol;    // volume element inside the branch
    GstElement       *amp;    // audioamplify, +POLARITY normal / -POLARITY inverted
    GstControlSource *vol_cs; // drives vol, switched sample-accurately on track change
    GstControlSource *amp_cs; // drives amp polarity, ramped to declick
    GstPad           *mixpad; // requested audiomixer sink pad
} Track;

// What the analysis of one file found. Built on a worker thread from nothing
// but a URI copy, then moved into the Track on the main thread.
typedef struct {
    GArray  *peaks;    // of Peak
    gint64   duration; // ns, -1 if nothing decoded
    double   lufs;
    double   dbtp;
    gboolean truncated;
    gboolean stalled;
} TrackAnalysis;

// Cheap: no decoding. Peaks stay empty and `analysing` set until an analysis
// result is moved in.
Track *track_new(const char *uri);
void   track_free(Track *t);

// The fixed analysis chain with `uri` set as a property; NULL on failure.
GstElement *track_analysis_pipeline(const char *uri, GstAppSink **sink_out);
// Analyse a running pipeline's appsink into a, within the given limits. Stops
// within ANALYZE_POLL of `cancel` being cancelled (which may be NULL).
void track_analyze(TrackAnalysis *a, const char *uri, GstElement *pipe, GstAppSink *sink,
                   guint64 max_frames, GstClockTime stall_timeout, GCancellable *cancel);
// Blocking decode + analysis of `uri`; safe on any thread. Never NULL.
TrackAnalysis *track_analysis_run(const char *uri, GCancellable *cancel);
void           track_analysis_free(TrackAnalysis *a);

// Runs track_analysis_run() on a worker thread; `cb` always fires on the
// default (main loop) context, whatever context is thread-default when this is
// called. Call it from the main thread. The worker only ever sees its own copy
// of `uri`.
void track_analysis_start(const char *uri, GCancellable *cancel, GAsyncReadyCallback cb,
                          gpointer user);
// NULL with G_IO_ERROR_CANCELLED once `cancel` was cancelled, even if the
// worker had already finished.
TrackAnalysis *track_analysis_finish(GAsyncResult *res, GError **err);
