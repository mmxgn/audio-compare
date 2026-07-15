#include "waveform.h"

typedef struct {
    Track          *track;
    gboolean        active;
    double          playhead; // 0..1
    WaveformClickFn on_click;
    gpointer        user;
} WfData;

static void draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer user)
{
    WfData *d   = user;
    double  mid = h / 2.0;
    double  amp = mid * 0.9;

    // background
    cairo_set_source_rgb(cr, 0.11, 0.11, 0.13);
    cairo_paint(cr);

    // waveform: one vertical line per pixel column
    GArray *peaks = d->track->peaks;
    if (peaks->len > 0) {
        cairo_set_source_rgb(cr, 0.30, 0.75, 0.72);
        cairo_set_line_width(cr, 1.0);
        for (int x = 0; x < w; x++) {
            guint i0 = (guint) ((gint64) x * peaks->len / w);
            guint i1 = (guint) ((gint64) (x + 1) * peaks->len / w);
            if (i1 <= i0)
                i1 = i0 + 1;
            if (i1 > peaks->len)
                i1 = peaks->len;
            float lo = 0, hi = 0;
            for (guint i = i0; i < i1; i++) {
                Peak p = g_array_index(peaks, Peak, i);
                if (p.min < lo)
                    lo = p.min;
                if (p.max > hi)
                    hi = p.max;
            }
            cairo_move_to(cr, x + 0.5, mid - hi * amp);
            cairo_line_to(cr, x + 0.5, mid - lo * amp);
            cairo_stroke(cr);
        }
    }

    // caption: filename, top-left, same colour as the waveform
    cairo_set_source_rgb(cr, 0.30, 0.75, 0.72);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    cairo_move_to(cr, 6, 15);
    cairo_show_text(cr, d->track->name);

    // playhead
    double px = d->playhead * w;
    cairo_set_source_rgb(cr, 0.90, 0.30, 0.30);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, px, 0);
    cairo_line_to(cr, px, h);
    cairo_stroke(cr);

    // active border
    if (d->active) {
        cairo_set_source_rgb(cr, 0.30, 0.60, 0.95);
        cairo_set_line_width(cr, 2.0);
        cairo_rectangle(cr, 1, 1, w - 2, h - 2);
        cairo_stroke(cr);
    }
}

static void on_pressed(GtkGestureClick *g, int n, double x, double y, gpointer user)
{
    GtkWidget *wf = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    WfData    *d  = g_object_get_data(G_OBJECT(wf), "wf");
    int        w  = gtk_widget_get_width(wf);
    if (d->on_click && w > 0)
        d->on_click(wf, CLAMP(x / w, 0.0, 1.0), d->user);
}

GtkWidget *waveform_new(Track *t, WaveformClickFn on_click, gpointer user)
{
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, -1, 80); // min height; grows with the window
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_vexpand(area, TRUE);

    WfData *d   = g_new0(WfData, 1);
    d->track    = t;
    d->on_click = on_click;
    d->user     = user;
    g_object_set_data_full(G_OBJECT(area), "wf", d, g_free);

    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw, d, NULL);

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), NULL);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(click));

    return area;
}

void waveform_set_active(GtkWidget *wf, gboolean active)
{
    WfData *d = g_object_get_data(G_OBJECT(wf), "wf");
    if (d->active != active) {
        d->active = active;
        gtk_widget_queue_draw(wf);
    }
}

void waveform_set_playhead(GtkWidget *wf, double frac)
{
    WfData *d   = g_object_get_data(G_OBJECT(wf), "wf");
    d->playhead = frac;
    gtk_widget_queue_draw(wf);
}
