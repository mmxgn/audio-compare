#pragma once

#include <gtk/gtk.h>

#include "track.h"

// Called when the pane is clicked; frac is the click x as 0..1 of the width.
typedef void (*WaveformClickFn)(GtkWidget *wf, double frac, gpointer user);

GtkWidget *waveform_new(Track *t, WaveformClickFn on_click, gpointer user);
void       waveform_set_active(GtkWidget *wf, gboolean active);
void       waveform_set_playhead(GtkWidget *wf, double frac); // 0..1
