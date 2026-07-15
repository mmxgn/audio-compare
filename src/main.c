#include <adwaita.h>
#include <gst/gst.h>

#include "track.h"
#include "waveform.h"

typedef struct {
    GtkWindow *win;
    GtkWidget *list;        // vertical GtkBox of waveform panes
    GtkWidget *placeholder; // shown while empty
    GPtrArray *tracks;      // Track*
    GPtrArray *waves;       // GtkWidget* drawing area, parallel to tracks
    GPtrArray *rows;        // GtkWidget* pane container, parallel to tracks
    int        active;      // -1 if none
    gboolean   playing;
} App;

static App app;

static void update_borders(void)
{
    for (guint i = 0; i < app.waves->len; i++)
        waveform_set_active(g_ptr_array_index(app.waves, i), (int) i == app.active);
}

static void set_active(int b)
{
    if (b < 0 || b >= (int) app.tracks->len)
        return;

    gint64 pos = 0;
    if (app.active >= 0) {
        Track *cur = g_ptr_array_index(app.tracks, app.active);
        pos        = track_position(cur);
        if (pos < 0)
            pos = 0;
        track_pause(cur);
    }
    app.active = b;
    Track *t   = g_ptr_array_index(app.tracks, b);
    track_seek(t, pos);
    if (app.playing)
        track_play(t);
    update_borders();
}

static void toggle_play(void)
{
    if (app.active < 0)
        return;
    Track *t    = g_ptr_array_index(app.tracks, app.active);
    app.playing = !app.playing;
    if (app.playing)
        track_play(t);
    else
        track_pause(t);
}

static void on_wave_click(GtkWidget *wf, double frac, gpointer user)
{
    guint i;
    if (!g_ptr_array_find(app.waves, wf, &i))
        return;
    set_active((int) i);
    Track *t = g_ptr_array_index(app.tracks, i);
    track_seek(t, (gint64) (frac * t->duration));
}

static void show_placeholder(void)
{
    app.placeholder = adw_status_page_new();
    adw_status_page_set_title(ADW_STATUS_PAGE(app.placeholder), "Drop audio files here");
    adw_status_page_set_description(ADW_STATUS_PAGE(app.placeholder),
                                    "or use Open. Space plays, Alt+Up/Down switches.");
    gtk_widget_set_vexpand(app.placeholder, TRUE);
    gtk_box_append(GTK_BOX(app.list), app.placeholder);
}

static void remove_track(GtkWidget *row)
{
    guint i;
    if (!g_ptr_array_find(app.rows, row, &i))
        return;

    gboolean was_active = ((int) i == app.active);
    track_free(g_ptr_array_index(app.tracks, i));
    gtk_box_remove(GTK_BOX(app.list), row);
    g_ptr_array_remove_index(app.tracks, i);
    g_ptr_array_remove_index(app.waves, i);
    g_ptr_array_remove_index(app.rows, i);

    if (app.tracks->len == 0) {
        app.active  = -1;
        app.playing = FALSE;
        show_placeholder();
    } else if (was_active) {
        app.active = -1; // set_active seeks the new track from position 0
        set_active(MIN(i, app.tracks->len - 1));
    } else if ((int) i < app.active) {
        app.active--;
    }
    update_borders();
}

static void on_close(GtkButton *b, gpointer row)
{
    remove_track(row);
}

static void add_track(const char *uri)
{
    Track *t = track_new(uri);
    g_ptr_array_add(app.tracks, t);

    GtkWidget *wf = waveform_new(t, on_wave_click, NULL);
    g_ptr_array_add(app.waves, wf);

    // Overlay a close button on the top-right of the pane.
    GtkWidget *row = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(row), wf);

    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(close, "osd");
    gtk_widget_add_css_class(close, "circular");
    gtk_widget_set_focusable(close, FALSE);
    gtk_widget_set_halign(close, GTK_ALIGN_END);
    gtk_widget_set_valign(close, GTK_ALIGN_START);
    gtk_widget_set_margin_top(close, 4);
    gtk_widget_set_margin_end(close, 4);
    gtk_widget_set_tooltip_text(close, t->name);
    g_signal_connect(close, "clicked", G_CALLBACK(on_close), row);
    gtk_overlay_add_overlay(GTK_OVERLAY(row), close);

    g_ptr_array_add(app.rows, row);
    gtk_box_append(GTK_BOX(app.list), row);

    if (app.placeholder) {
        gtk_box_remove(GTK_BOX(app.list), app.placeholder);
        app.placeholder = NULL;
    }
    if (app.active < 0)
        set_active(0);
}

static gboolean tick(gpointer user)
{
    if (app.active >= 0) {
        Track *t   = g_ptr_array_index(app.tracks, app.active);
        gint64 pos = track_position(t);
        if (pos >= 0) {
            for (guint i = 0; i < app.waves->len; i++) {
                Track *ti   = g_ptr_array_index(app.tracks, i);
                double frac = ti->duration > 0 ? (double) pos / ti->duration : 0;
                waveform_set_playhead(g_ptr_array_index(app.waves, i), frac);
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType state, gpointer user)
{
    if (keyval == GDK_KEY_space) {
        toggle_play();
        return TRUE;
    }
    if (state & GDK_ALT_MASK) {
        if (keyval == GDK_KEY_Up) {
            set_active(app.active - 1);
            return TRUE;
        }
        if (keyval == GDK_KEY_Down) {
            set_active(app.active + 1);
            return TRUE;
        }
    }
    return FALSE;
}

static void on_files_chosen(GObject *src, GAsyncResult *res, gpointer user)
{
    GError    *err   = NULL;
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, &err);
    if (!files) {
        g_clear_error(&err);
        return;
    }
    guint n = g_list_model_get_n_items(files);
    for (guint i = 0; i < n; i++) {
        GFile *f   = g_list_model_get_item(files, i);
        char  *uri = g_file_get_uri(f);
        add_track(uri);
        g_free(uri);
        g_object_unref(f);
    }
    g_object_unref(files);
}

static void on_open(GtkButton *b, gpointer user)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_open_multiple(d, app.win, NULL, on_files_chosen, NULL);
    g_object_unref(d);
}

static gboolean on_drop(GtkDropTarget *t, const GValue *value, double x, double y, gpointer user)
{
    if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
        return FALSE;
    GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
    for (GSList *l = files; l; l = l->next) {
        char *uri = g_file_get_uri(l->data);
        add_track(uri);
        g_free(uri);
    }
    g_slist_free(files);
    return TRUE;
}

static void activate(GtkApplication *gapp, gpointer user)
{
    app.tracks = g_ptr_array_new();
    app.waves  = g_ptr_array_new();
    app.rows   = g_ptr_array_new();
    app.active = -1;

    GtkWidget *win = adw_application_window_new(gapp);
    app.win        = GTK_WINDOW(win);
    gtk_window_set_title(app.win, "Audio Compare");
    gtk_window_set_default_size(app.win, 800, 600);

    GtkWidget *open = gtk_button_new_with_label("Open");
    g_signal_connect(open, "clicked", G_CALLBACK(on_open), NULL);

    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), open);

    app.list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(app.list, 6);
    gtk_widget_set_margin_bottom(app.list, 6);
    gtk_widget_set_margin_start(app.list, 6);
    gtk_widget_set_margin_end(app.list, 6);

    show_placeholder();

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), app.list);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);

    GtkWidget *toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), toolbar);

    GtkEventController *keys = gtk_event_controller_key_new();
    // Capture phase: intercept Space before the focused Open button consumes it.
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), NULL);
    gtk_widget_add_controller(win, keys);

    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(on_drop), NULL);
    gtk_widget_add_controller(win, GTK_EVENT_CONTROLLER(drop));

    g_timeout_add(33, tick, NULL);

    gtk_window_present(app.win);
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    AdwApplication *a = adw_application_new("org.mmxgn.audiocompare", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(a, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(a), argc, argv);
    g_object_unref(a);
    return status;
}
