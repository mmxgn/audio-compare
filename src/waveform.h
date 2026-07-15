#pragma once

#include <gtk/gtk.h>

#include "track.h"

// Called when the pane is clicked; frac is the click x as 0..1 of the width.
typedef void (*WaveformClickFn)(GtkWidget *wf, double frac, gpointer user);
// Called at the start (active=TRUE) and end (active=FALSE) of a scrub drag.
typedef void (*WaveformScrubFn)(GtkWidget *wf, gboolean active, gpointer user);

GtkWidget *waveform_new(Track *t, WaveformClickFn on_click, WaveformScrubFn on_scrub,
                        gpointer user);
void       waveform_set_active(GtkWidget *wf, gboolean active);
void       waveform_set_dimmed(GtkWidget *wf, gboolean dimmed); // grey out (muted/solo-suppressed)
void       waveform_set_playhead(GtkWidget *wf, double frac);   // 0..1
