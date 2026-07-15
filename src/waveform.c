#include <adwaita.h>
#include <math.h>

#include "waveform.h"

// Distinct colour per bus 0-9, for borders and the numbered badge.
static const double BUS_PALETTE[10][3] = {
    { 0.88, 0.11, 0.14 }, { 1.00, 0.47, 0.00 }, { 0.96, 0.76, 0.06 }, { 0.20, 0.82, 0.48 },
    { 0.13, 0.83, 0.83 }, { 0.21, 0.52, 0.89 }, { 0.49, 0.31, 0.77 }, { 0.96, 0.42, 0.69 },
    { 0.60, 0.42, 0.27 }, { 0.60, 0.60, 0.59 },
};

typedef struct {
    Track          *track;
    gboolean        active;
    double          playhead; // 0..1
    double          drag_x;   // press x, for drag scrubbing
    WaveformClickFn on_click;
    gpointer        user;
} WfData;

static void
draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer user)
{
    WfData *d   = user;
    double  mid = h / 2.0;
    double  amp = mid * 0.9;

    // Themed background comes from the "view" CSS class; content uses the
    // accent colour so it follows the light/dark scheme. The accent API needs
    // libadwaita 1.6; older versions fall back to the theme foreground colour.
    GdkRGBA acc;
#if ADW_CHECK_VERSION(1, 6, 0)
    AdwStyleManager *sm = adw_style_manager_get_default();
    adw_accent_color_to_standalone_rgba(adw_style_manager_get_accent_color(sm),
                                        adw_style_manager_get_dark(sm), &acc);
#else
    gtk_widget_get_color(GTK_WIDGET(area), &acc);
#endif

    // waveform: one vertical line per pixel column
    GArray *peaks = d->track->peaks;
    if (peaks->len > 0) {
        cairo_set_source_rgb(cr, acc.red, acc.green, acc.blue);
        cairo_set_line_width(cr, 1.0);
        for (int x = 0; x < w; x++) {
            guint i0 = (guint)((gint64)x * peaks->len / w);
            guint i1 = (guint)((gint64)(x + 1) * peaks->len / w);
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
    cairo_set_source_rgb(cr, acc.red, acc.green, acc.blue);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    cairo_move_to(cr, 6, 15);
    cairo_show_text(cr, d->track->name);

    // loudness stats, top-right (leaving room for the close button)
    char tp[24], lu[24], stats[52];
    if (isfinite(d->track->dbtp))
        g_snprintf(tp, sizeof tp, "%.1f dBTP", d->track->dbtp);
    else
        g_strlcpy(tp, "-inf dBTP", sizeof tp);
    if (isfinite(d->track->lufs))
        g_snprintf(lu, sizeof lu, "%.1f LUFS", d->track->lufs);
    else
        g_strlcpy(lu, "-inf LUFS", sizeof lu);
    g_snprintf(stats, sizeof stats, "%s   %s", tp, lu);

    cairo_text_extents_t ext;
    cairo_text_extents(cr, stats, &ext);
    cairo_move_to(cr, w - 44 - ext.width, 15);
    cairo_show_text(cr, stats);

    // playhead
    double px = d->playhead * w;
    cairo_set_source_rgb(cr, 0.90, 0.30, 0.30);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, px, 0);
    cairo_line_to(cr, px, h);
    cairo_stroke(cr);

    // time label following the playhead: min'sec''ms'''
    if (d->track->duration > 0) {
        gint64 pos = (gint64)(d->playhead * (double)d->track->duration) / GST_MSECOND;
        char   tbuf[32];
        g_snprintf(tbuf, sizeof tbuf, "%d'%02d''%03d'''", (int)(pos / 60000),
                   (int)(pos / 1000 % 60), (int)(pos % 1000));

        cairo_text_extents_t te;
        cairo_text_extents(cr, tbuf, &te);
        double pad = 4, bw = te.width + 2 * pad, bh = 11 + 2 * pad;
        double bx = px + 3 + bw > w ? px - 3 - bw : px + 3;
        double by = mid - bh / 2;

        cairo_set_source_rgb(cr, 0.90, 0.30, 0.30);
        cairo_rectangle(cr, bx, by, bw, bh);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_move_to(cr, bx + pad - te.x_bearing, by + pad - te.y_bearing);
        cairo_show_text(cr, tbuf);
    }

    // border: bus colour if grouped (else accent), thick when audible/active
    int      bus     = d->track->bus;
    gboolean grouped = bus >= 0 && bus < 10;
    if (grouped || d->active) {
        if (grouped)
            cairo_set_source_rgb(cr, BUS_PALETTE[bus][0], BUS_PALETTE[bus][1], BUS_PALETTE[bus][2]);
        else
            cairo_set_source_rgb(cr, acc.red, acc.green, acc.blue);
        double lw = d->active ? 4.0 : 2.0;
        cairo_set_line_width(cr, lw);
        cairo_rectangle(cr, lw / 2, lw / 2, w - lw, h - lw);
        cairo_stroke(cr);
    }

    // bus badge: the digit in a filled box, bottom-left
    if (grouped) {
        char label[2] = { (char)('0' + bus), 0 };
        cairo_set_font_size(cr, 12);
        cairo_text_extents_t be;
        cairo_text_extents(cr, label, &be);
        double pad = 5, bw = be.width + 2 * pad, bh = 12 + 2 * pad;
        double bx = 6, by = h - 6 - bh;

        cairo_set_source_rgb(cr, BUS_PALETTE[bus][0], BUS_PALETTE[bus][1], BUS_PALETTE[bus][2]);
        cairo_rectangle(cr, bx, by, bw, bh);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_move_to(cr, bx + pad - be.x_bearing, by + pad - be.y_bearing);
        cairo_show_text(cr, label);
    }
}

static void
scrub_to(GtkWidget *wf, double x)
{
    WfData *d = g_object_get_data(G_OBJECT(wf), "wf");
    int     w = gtk_widget_get_width(wf);
    if (d->on_click && w > 0)
        d->on_click(wf, CLAMP(x / w, 0.0, 1.0), d->user);
}

static void
on_drag_begin(GtkGestureDrag *g, double sx, double sy, gpointer user)
{
    GtkWidget *wf = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    WfData    *d  = g_object_get_data(G_OBJECT(wf), "wf");
    d->drag_x     = sx;
    scrub_to(wf, sx);
}

static void
on_drag_update(GtkGestureDrag *g, double ox, double oy, gpointer user)
{
    GtkWidget *wf = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    WfData    *d  = g_object_get_data(G_OBJECT(wf), "wf");
    scrub_to(wf, d->drag_x + ox);
}

GtkWidget *
waveform_new(Track *t, WaveformClickFn on_click, gpointer user)
{
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, -1, 80); // min height; grows with the window
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_vexpand(area, TRUE);
    gtk_widget_add_css_class(area, "view"); // themed light/dark background

    // Redraw when the scheme or accent colour changes.
    AdwStyleManager *sm = adw_style_manager_get_default();
    g_signal_connect_object(sm, "notify::dark", G_CALLBACK(gtk_widget_queue_draw), area,
                            G_CONNECT_SWAPPED);
#if ADW_CHECK_VERSION(1, 6, 0)
    g_signal_connect_object(sm, "notify::accent-color", G_CALLBACK(gtk_widget_queue_draw), area,
                            G_CONNECT_SWAPPED);
#endif

    WfData *d   = g_new0(WfData, 1);
    d->track    = t;
    d->on_click = on_click;
    d->user     = user;
    g_object_set_data_full(G_OBJECT(area), "wf", d, g_free);

    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw, d, NULL);

    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), NULL);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(drag));

    return area;
}

void
waveform_set_active(GtkWidget *wf, gboolean active)
{
    WfData *d = g_object_get_data(G_OBJECT(wf), "wf");
    if (d->active != active) {
        d->active = active;
        gtk_widget_queue_draw(wf);
    }
}

void
waveform_set_playhead(GtkWidget *wf, double frac)
{
    WfData *d   = g_object_get_data(G_OBJECT(wf), "wf");
    d->playhead = frac;
    gtk_widget_queue_draw(wf);
}
