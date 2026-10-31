#include "ui.h"
#include "auth.h"
#include "player.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Keys:
 *   Space        play / pause
 *   Left / Right previous / next track
 *   Up / Down    choose a playlist
 *   Enter        play the chosen playlist (double-click works too)
 *   + / -        volume up / down
 *   S            shuffle on / off
 *   F5           reload playlists
 *   Esc or Q     quit
 *
 * Every Spotify call runs on a worker thread so the window never freezes.
 * api_lock makes those calls take turns (the token is shared).
 */

static GMutex api_lock;

static struct {
    GtkWidget    *win;
    GtkWidget    *track, *artist, *info, *time, *bar, *status, *tree;
    GtkListStore *store;

    int  logged_in;
    int  poll_busy;
    int  playing, shuffle, volume;   /* volume -1 = unknown */
    long prog_ms, dur_ms;
    int    seeking;      /* user is dragging the bar */
    gint64 seek_until;   /* ignore old positions from the server until then */

    GtkListStore *tracks;       /* songs of the playlist shown with T */
    GtkWidget    *list_title;
    int  view_tracks;           /* 1 = list shows songs, 0 = playlists */
    int  tracks_busy;
    char context_id[64];        /* playlist that is playing right now */
    char current_uri[96];       /* song that is playing right now */
    char tracks_pl[64];         /* playlist whose songs are in `tracks` */
    char sel_pl[64];            /* playlist that was selected before pressing T */
} ui;

static void select_current_track(void);

/* ---------- small helpers (main thread only) ---------- */

static void set_status(const char *msg) {
    gtk_label_set_text(GTK_LABEL(ui.status), msg);
}

static const char *explain(long s) {
    static char buf[64];
    if (s >= 200 && s < 300) return NULL;

    switch (s) {
    case -1:  return "Network error";
    case 401: return "Login expired - restart the app";
    case 403: return "Spotify refused (playback control needs Premium)";
    case 404: return "No active device - open Spotify and play something";
    case 429: return "Rate limited - wait a moment";
    }
    snprintf(buf, sizeof buf, "Spotify error (HTTP %ld)", s);
    return buf;
}

static void update_info(void) {
    char vol[24], buf[128];

    if (ui.volume >= 0) snprintf(vol, sizeof vol, "Volume %d%%", ui.volume);
    else                snprintf(vol, sizeof vol, "Volume n/a");

    snprintf(buf, sizeof buf, "%s   |   %s   |   Shuffle %s",
             ui.playing ? "Playing" : "Paused", vol, ui.shuffle ? "on" : "off");
    gtk_label_set_text(GTK_LABEL(ui.info), buf);
}

static void update_time(void) {
    if (ui.seeking) return;
    char buf[64];
    long p = ui.prog_ms / 1000, d = ui.dur_ms / 1000;

    snprintf(buf, sizeof buf, "%ld:%02ld / %ld:%02ld", p / 60, p % 60, d / 60, d % 60);
    gtk_label_set_text(GTK_LABEL(ui.time), buf);

    double frac = ui.dur_ms > 0 ? (double)ui.prog_ms / (double)ui.dur_ms : 0.0;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    gtk_range_set_value(GTK_RANGE(ui.bar), frac * 1000.0);
}

/* ---------- polling the current playback state ---------- */

typedef struct {
    PlayerState st;
    long        status;
} PollResult;

static gboolean poll_done(gpointer data) {
    PollResult *r = data;
    ui.poll_busy = 0;

    if (r->status == 200 && r->st.active) {
        char *m = g_markup_printf_escaped(
            "<span size=\"x-large\" weight=\"bold\">%s</span>", r->st.track);
        gtk_label_set_markup(GTK_LABEL(ui.track), m);
        g_free(m);

        gtk_label_set_text(GTK_LABEL(ui.artist), r->st.artist);

        int song_changed = strcmp(ui.current_uri, r->st.track_uri) != 0;
        snprintf(ui.current_uri, sizeof ui.current_uri, "%s", r->st.track_uri);
        snprintf(ui.context_id, sizeof ui.context_id, "%s", r->st.context_id);
        if (song_changed && ui.view_tracks &&
            strcmp(ui.tracks_pl, ui.context_id) == 0)
            select_current_track();

        ui.playing = r->st.playing;
        ui.shuffle = r->st.shuffle;
        ui.volume  = r->st.volume;
        if (g_get_monotonic_time() >= ui.seek_until)
            ui.prog_ms = r->st.progress_ms;
        ui.dur_ms  = r->st.duration_ms;
        update_info();
        update_time();
    } else if (r->status == 200 || r->status == 204) {
        gtk_label_set_markup(GTK_LABEL(ui.track),
            "<span size=\"x-large\" weight=\"bold\">Nothing playing</span>");
        gtk_label_set_text(GTK_LABEL(ui.artist),
                           "Open Spotify on a device, then pick a playlist");
        gtk_label_set_text(GTK_LABEL(ui.info), "");
        ui.playing = 0;
        ui.prog_ms = ui.dur_ms = 0;
        update_time();
    } else {
        const char *e = explain(r->status);
        if (e) set_status(e);
    }

    g_free(r);
    return G_SOURCE_REMOVE;
}

static gpointer poll_thread(gpointer data) {
    PollResult *r = data;
    g_mutex_lock(&api_lock);
    r->status = player_get_state(&r->st);
    g_mutex_unlock(&api_lock);
    g_idle_add(poll_done, r);
    return NULL;
}

static void poll_now(void) {
    if (!ui.logged_in || ui.poll_busy) return;
    ui.poll_busy = 1;

    GThread *t = g_thread_new("poll", poll_thread, g_new0(PollResult, 1));
    g_thread_unref(t);
}

static gboolean poll_timer_cb(gpointer unused) {
    poll_now();
    return G_SOURCE_CONTINUE;
}

static gboolean poll_once_cb(gpointer unused) {
    poll_now();
    return G_SOURCE_REMOVE;
}

/* Moves the progress bar between polls so it looks smooth. */
static gboolean tick_cb(gpointer unused) {
    if (ui.logged_in && ui.playing && ui.dur_ms > 0) {
        ui.prog_ms += 500;
        if (ui.prog_ms > ui.dur_ms) ui.prog_ms = ui.dur_ms;
        update_time();
    }
    return G_SOURCE_CONTINUE;
}

/* ---------- commands (play, pause, next, ...) ---------- */

typedef struct {
    int  playlist;          /* 1 = play playlist `id`, 0 = plain http command */
    char method[8];
    char path[128];
    char id[64];
    char uri[96];
    int  track;             /* 1 = play song `uri` inside playlist `id` */
    char okmsg[192];
    long status;
} Job;

static gboolean job_done(gpointer data) {
    Job *j = data;
    const char *err = explain(j->status);

    if (err) {
        set_status(err);
    } else {
        set_status(j->okmsg);
        g_timeout_add(600, poll_once_cb, NULL);   /* Spotify needs a moment */
    }

    g_free(j);
    return G_SOURCE_REMOVE;
}

static gpointer job_thread(gpointer data) {
    Job *j = data;

    g_mutex_lock(&api_lock);
    if (j->track)         j->status = player_play_track_in_playlist(j->id, j->uri);
    else if (j->playlist) j->status = player_play_playlist_id(j->id);
    else                  j->status = player_http_cmd(j->method, j->path);
    g_mutex_unlock(&api_lock);

    g_idle_add(job_done, j);
    return NULL;
}

static void start_job(Job *j) {
    GThread *t = g_thread_new("job", job_thread, j);
    g_thread_unref(t);
}

static void send_cmd(const char *method, const char *path, const char *okmsg) {
    if (!ui.logged_in) return;

    Job *j = g_new0(Job, 1);
    snprintf(j->method, sizeof j->method, "%s", method);
    snprintf(j->path, sizeof j->path, "%s", path);
    snprintf(j->okmsg, sizeof j->okmsg, "%s", okmsg);
    start_job(j);
}

static void play_selected(void) {
    if (!ui.logged_in) return;

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ui.tree));
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, NULL, &it)) {
        set_status("Pick a playlist first (Up / Down)");
        return;
    }

    gchar *name = NULL, *id = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(ui.store), &it, 0, &name, 1, &id, -1);

    Job *j = g_new0(Job, 1);
    j->playlist = 1;
    snprintf(j->id, sizeof j->id, "%s", id ? id : "");
    snprintf(j->okmsg, sizeof j->okmsg, "Playing: %s", name ? name : "");
    start_job(j);

    g_free(name);
    g_free(id);
}

static void change_volume(int delta) {
    if (!ui.logged_in) return;
    int v = (ui.volume >= 0 ? ui.volume : 50) + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;

    ui.volume = v;
    update_info();

    char path[64], msg[32];
    snprintf(path, sizeof path, "/volume?volume_percent=%d", v);
    snprintf(msg, sizeof msg, "Volume %d%%", v);
    send_cmd("PUT", path, msg);
}

static void toggle_shuffle(void) {
    if (!ui.logged_in) return;
    ui.shuffle = !ui.shuffle;
    update_info();

    send_cmd("PUT", ui.shuffle ? "/shuffle?state=true" : "/shuffle?state=false",
             ui.shuffle ? "Shuffle on" : "Shuffle off");
}

static void toggle_play(void) {
    if (!ui.logged_in) return;
    if (ui.playing) {
        ui.playing = 0;
        update_info();
        send_cmd("PUT", "/pause", "Paused");
    } else {
        ui.playing = 1;
        update_info();
        send_cmd("PUT", "/play", "Playing");
    }
}

/* ---------- playlists ---------- */

typedef struct {
    PlaylistInfo *items;
    int           n;
    long          status;
} PlResult;

static gboolean playlists_done(gpointer data) {
    PlResult *r = data;

    if (r->status != 200) {
        const char *e = explain(r->status);
        set_status(e ? e : "Could not load playlists");
    } else {
        gtk_list_store_clear(ui.store);

        for (int i = 0; i < r->n; i++) {
            GtkTreeIter it;
            gtk_list_store_append(ui.store, &it);
            gtk_list_store_set(ui.store, &it, 0, r->items[i].name,
                               1, r->items[i].id, -1);
        }

        if (r->n > 0 && !ui.view_tracks) {
            GtkTreeIter first;
            if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ui.store), &first))
                gtk_tree_selection_select_iter(
                    gtk_tree_view_get_selection(GTK_TREE_VIEW(ui.tree)), &first);
        }

        char buf[64];
        snprintf(buf, sizeof buf, "Loaded %d playlist%s", r->n, r->n == 1 ? "" : "s");
        set_status(buf);
    }

    free(r->items);
    g_free(r);
    return G_SOURCE_REMOVE;
}

static gpointer playlists_thread(gpointer data) {
    PlResult *r = data;
    g_mutex_lock(&api_lock);
    r->status = player_fetch_playlists(&r->items, &r->n);
    g_mutex_unlock(&api_lock);
    g_idle_add(playlists_done, r);
    return NULL;
}

static void load_playlists(void) {
    if (!ui.logged_in) return;
    set_status("Loading playlists...");

    GThread *t = g_thread_new("playlists", playlists_thread, g_new0(PlResult, 1));
    g_thread_unref(t);
}

static void move_selection(int delta) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ui.tree));
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(ui.tree));
    GtkTreeIter it;

    if (!gtk_tree_selection_get_selected(sel, NULL, &it)) {
        if (!gtk_tree_model_get_iter_first(model, &it)) return;
    } else if (delta > 0) {
        if (!gtk_tree_model_iter_next(model, &it)) return;
    } else {
        if (!gtk_tree_model_iter_previous(model, &it)) return;
    }

    gtk_tree_selection_select_iter(sel, &it);

    GtkTreePath *path = gtk_tree_model_get_path(model, &it);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(ui.tree), path, NULL, FALSE, 0, 0);
    gtk_tree_path_free(path);
}

/* ---------- songs of the playing playlist (T key) ---------- */

static void select_row_by_id(GtkListStore *store, const char *id, gboolean center) {
    GtkTreeModel *model = GTK_TREE_MODEL(store);
    GtkTreeIter it;

    if (!id || !id[0] || !gtk_tree_model_get_iter_first(model, &it)) return;

    do {
        gchar *rid = NULL;
        gtk_tree_model_get(model, &it, 1, &rid, -1);
        int match = rid && strcmp(rid, id) == 0;
        g_free(rid);

        if (match) {
            gtk_tree_selection_select_iter(
                gtk_tree_view_get_selection(GTK_TREE_VIEW(ui.tree)), &it);

            GtkTreePath *path = gtk_tree_model_get_path(model, &it);
            gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(ui.tree), path, NULL,
                                         center, 0.5f, 0.0f);
            gtk_tree_path_free(path);
            return;
        }
    } while (gtk_tree_model_iter_next(model, &it));
}

static void select_current_track(void) {
    select_row_by_id(ui.tracks, ui.current_uri, TRUE);
}

static void remember_playlist_selection(void) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ui.tree));
    GtkTreeIter it;

    if (gtk_tree_selection_get_selected(sel, NULL, &it)) {
        gchar *id = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(ui.store), &it, 1, &id, -1);
        snprintf(ui.sel_pl, sizeof ui.sel_pl, "%s", id ? id : "");
        g_free(id);
    }
}

typedef struct {
    TrackInfo *items;
    int        n;
    long       status;
    char       playlist_id[64];
} TrkResult;

static gboolean tracks_done(gpointer data) {
    TrkResult *r = data;
    ui.tracks_busy = 0;

    if (r->status != 200) {
        set_status(r->status == 403 || r->status == 404
                       ? "Spotify won't show the songs of this playlist"
                       : explain(r->status));
    } else if (r->n == 0) {
        set_status("No songs available - Spotify only lets this app read your own playlists");
    } else {
        gtk_list_store_clear(ui.tracks);

        for (int i = 0; i < r->n; i++) {
            char label[600];
            if (r->items[i].artist[0])
                snprintf(label, sizeof label, "%s  -  %s",
                         r->items[i].name, r->items[i].artist);
            else
                snprintf(label, sizeof label, "%s", r->items[i].name);

            GtkTreeIter it;
            gtk_list_store_append(ui.tracks, &it);
            gtk_list_store_set(ui.tracks, &it, 0, label, 1, r->items[i].uri, -1);
        }

        snprintf(ui.tracks_pl, sizeof ui.tracks_pl, "%s", r->playlist_id);

        char title[320] = "Songs in the playing playlist   (T = back)";
        GtkTreeModel *pm = GTK_TREE_MODEL(ui.store);
        GtkTreeIter pit;
        if (gtk_tree_model_get_iter_first(pm, &pit)) {
            do {
                gchar *pid = NULL, *pname = NULL;
                gtk_tree_model_get(pm, &pit, 0, &pname, 1, &pid, -1);
                if (pid && pname && strcmp(pid, r->playlist_id) == 0)
                    snprintf(title, sizeof title, "Songs in: %s   (T = back)", pname);
                g_free(pid);
                g_free(pname);
            } while (gtk_tree_model_iter_next(pm, &pit));
        }

        ui.view_tracks = 1;
        gtk_tree_view_set_model(GTK_TREE_VIEW(ui.tree), GTK_TREE_MODEL(ui.tracks));
        gtk_label_set_text(GTK_LABEL(ui.list_title), title);
        select_current_track();
        set_status("Showing songs - press T to go back");
    }

    free(r->items);
    g_free(r);
    return G_SOURCE_REMOVE;
}

static gpointer tracks_thread(gpointer data) {
    TrkResult *r = data;
    g_mutex_lock(&api_lock);
    r->status = player_fetch_playlist_tracks(r->playlist_id, &r->items, &r->n);
    g_mutex_unlock(&api_lock);
    g_idle_add(tracks_done, r);
    return NULL;
}

static void toggle_tracks_view(void) {
    if (!ui.logged_in) return;

    if (ui.view_tracks) {                       /* back to the playlists */
        ui.view_tracks = 0;
        gtk_tree_view_set_model(GTK_TREE_VIEW(ui.tree), GTK_TREE_MODEL(ui.store));
        gtk_label_set_text(GTK_LABEL(ui.list_title), "Your playlists");
        select_row_by_id(ui.store, ui.sel_pl, TRUE);
        set_status("Playlists");
        return;
    }

    if (!ui.context_id[0]) {
        set_status("Not playing from one of your playlists");
        return;
    }
    if (ui.tracks_busy) return;

    remember_playlist_selection();
    ui.tracks_busy = 1;
    set_status("Loading songs...");

    TrkResult *r = g_new0(TrkResult, 1);
    snprintf(r->playlist_id, sizeof r->playlist_id, "%s", ui.context_id);

    GThread *t = g_thread_new("tracks", tracks_thread, r);
    g_thread_unref(t);
}

static void play_selected_track(void) {
    if (!ui.logged_in) return;

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ui.tree));
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, NULL, &it)) {
        set_status("Pick a song first (Up / Down)");
        return;
    }

    gchar *name = NULL, *uri = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(ui.tracks), &it, 0, &name, 1, &uri, -1);

    Job *j = g_new0(Job, 1);
    j->track = 1;
    snprintf(j->id, sizeof j->id, "%s", ui.tracks_pl);
    snprintf(j->uri, sizeof j->uri, "%s", uri ? uri : "");
    snprintf(j->okmsg, sizeof j->okmsg, "Playing: %s", name ? name : "");
    start_job(j);

    g_free(name);
    g_free(uri);
}

static void activate_selected(void) {
    if (ui.view_tracks) play_selected_track();
    else                play_selected();
}

/* ---------- login ---------- */

static gboolean auth_done(gpointer data) {
    if (GPOINTER_TO_INT(data) != 0) {
        set_status("Login failed - close the app and try again");
        return G_SOURCE_REMOVE;
    }

    ui.logged_in = 1;
    set_status("Logged in");
    load_playlists();
    poll_now();
    g_timeout_add(2000, poll_timer_cb, NULL);
    return G_SOURCE_REMOVE;
}

static gpointer auth_thread(gpointer data) {
    g_mutex_lock(&api_lock);
    int rc = auth_init();           /* may open the browser and wait */
    g_mutex_unlock(&api_lock);
    g_idle_add(auth_done, GINT_TO_POINTER(rc));
    return NULL;
}

/* ---------- keyboard ---------- */

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer unused) {
    switch (ev->keyval) {
    case GDK_KEY_Escape:
    case GDK_KEY_q:
    case GDK_KEY_Q:
        gtk_main_quit();
        return TRUE;

    case GDK_KEY_space:
        toggle_play();
        return TRUE;

    case GDK_KEY_Right:
        send_cmd("POST", "/next", "Next track");
        return TRUE;

    case GDK_KEY_Left:
        send_cmd("POST", "/previous", "Previous track");
        return TRUE;

    case GDK_KEY_Up:
        move_selection(-1);
        return TRUE;

    case GDK_KEY_Down:
        move_selection(+1);
        return TRUE;

    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        activate_selected();
        return TRUE;

    case GDK_KEY_plus:
    case GDK_KEY_equal:
    case GDK_KEY_KP_Add:
        change_volume(+10);
        return TRUE;

    case GDK_KEY_minus:
    case GDK_KEY_KP_Subtract:
        change_volume(-10);
        return TRUE;

    case GDK_KEY_s:
    case GDK_KEY_S:
        toggle_shuffle();
        return TRUE;

    case GDK_KEY_t:
    case GDK_KEY_T:
        toggle_tracks_view();
        return TRUE;

    case GDK_KEY_F5:
        load_playlists();
        return TRUE;
    }
    return FALSE;
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path,
                             GtkTreeViewColumn *col, gpointer unused) {
    activate_selected();
}

/* ---------- seek bar ---------- */

static gboolean on_scale_press(GtkWidget *w, GdkEventButton *ev, gpointer unused) {
    if (ui.logged_in && ui.dur_ms > 0) ui.seeking = 1;
    return FALSE;
}

/* While dragging, show the time under the handle. */
static void on_scale_changed(GtkRange *range, gpointer unused) {
    if (!ui.seeking || ui.dur_ms <= 0) return;

    long p = (long)(gtk_range_get_value(range) / 1000.0 * (double)ui.dur_ms) / 1000;
    long d = ui.dur_ms / 1000;
    char buf[64];
    snprintf(buf, sizeof buf, "%ld:%02ld / %ld:%02ld", p / 60, p % 60, d / 60, d % 60);
    gtk_label_set_text(GTK_LABEL(ui.time), buf);
}

static gboolean on_scale_release(GtkWidget *w, GdkEventButton *ev, gpointer unused) {
    if (!ui.seeking) return FALSE;
    ui.seeking = 0;

    long pos = (long)(gtk_range_get_value(GTK_RANGE(w)) / 1000.0 * (double)ui.dur_ms);
    ui.prog_ms = pos;
    ui.seek_until = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    update_time();

    char path[64];
    snprintf(path, sizeof path, "/seek?position_ms=%ld", pos);
    send_cmd("PUT", path, "Jumped to new position");
    return FALSE;
}

/* ---------- window ---------- */

static GtkWidget *make_label(const char *text, int dim) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    if (dim)
        gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim");
    return l;
}

static void apply_css(void) {
    static const char *css_text =
        "window { background-color: #121212; }"
        "label { color: #ffffff; }"
        ".dim { color: #b3b3b3; }"
        "treeview.view { background-color: #181818; color: #ffffff; font-size: 14px; }"
        "treeview.view:selected { background-color: #1db954; color: #000000; }"
        "scale trough { min-height: 6px; background-color: #404040; border: none; }"
        "scale highlight { background-color: #1db954; border: none; }"
        "scale slider { background-color: #ffffff; background-image: none; border: none; }";

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, css_text, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}
static gboolean on_window_delete(GtkWidget *widget,GdkEvent *event,gpointer data) {
    fprintf(stderr, "DEBUG: DELETE EVENT RECEIVED!\n");
    return TRUE;  // Temporarily prevent the window from closing
}

static void on_window_destroy(GtkWidget *widget, gpointer data) {
    fprintf(stderr, "DEBUG: WINDOW DESTROYED!\n");
    gtk_main_quit();
}
int ui_run(void) {
    gtk_init(NULL, NULL);

    ui.volume = -1;
    apply_css();

    ui.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(ui.win), "Spotify Controller");
    gtk_window_set_default_size(GTK_WINDOW(ui.win), 460, 640);
    g_signal_connect(ui.win, "delete-event",G_CALLBACK(on_window_delete), NULL);

    g_signal_connect(ui.win, "destroy",G_CALLBACK(on_window_destroy), NULL);
    g_signal_connect(ui.win, "key-press-event", G_CALLBACK(on_key), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_container_add(GTK_CONTAINER(ui.win), box);

    ui.track = make_label("", 0);
    gtk_label_set_markup(GTK_LABEL(ui.track),
        "<span size=\"x-large\" weight=\"bold\">Spotify Controller</span>");
    gtk_box_pack_start(GTK_BOX(box), ui.track, FALSE, FALSE, 0);

    ui.artist = make_label("Logging in... approve access in your browser", 1);
    gtk_box_pack_start(GTK_BOX(box), ui.artist, FALSE, FALSE, 0);

    ui.bar = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1000, 1);
    gtk_scale_set_draw_value(GTK_SCALE(ui.bar), FALSE);
    g_signal_connect(ui.bar, "button-press-event", G_CALLBACK(on_scale_press), NULL);
    g_signal_connect_after(ui.bar, "button-release-event", G_CALLBACK(on_scale_release), NULL);
    g_signal_connect(ui.bar, "value-changed", G_CALLBACK(on_scale_changed), NULL);
    gtk_box_pack_start(GTK_BOX(box), ui.bar, FALSE, FALSE, 6);

    ui.time = make_label("0:00 / 0:00", 1);
    gtk_box_pack_start(GTK_BOX(box), ui.time, FALSE, FALSE, 0);

    ui.info = make_label("", 1);
    gtk_box_pack_start(GTK_BOX(box), ui.info, FALSE, FALSE, 0);

    ui.status = make_label("", 1);
    gtk_box_pack_start(GTK_BOX(box), ui.status, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 6);

    ui.list_title = make_label("Your playlists", 1);
    gtk_box_pack_start(GTK_BOX(box), ui.list_title, FALSE, FALSE, 0);

    ui.store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    ui.tracks = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    ui.tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ui.store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(ui.tree), FALSE);
    g_signal_connect(ui.tree, "row-activated", G_CALLBACK(on_row_activated), NULL);

    GtkCellRenderer *cell = gtk_cell_renderer_text_new();
    g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 6, NULL);
    GtkTreeViewColumn *col =
        gtk_tree_view_column_new_with_attributes("Name", cell, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(ui.tree), col);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), ui.tree);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    GtkWidget *hint = make_label(
        "Space play/pause   Left/Right prev/next   Up/Down choose playlist\n"
        "Enter play playlist   +/- volume   S shuffle   F5 reload   Esc quit\n"
        "Click or drag the bar to seek   T songs of the playing playlist", 1);
    gtk_label_set_ellipsize(GTK_LABEL(hint), PANGO_ELLIPSIZE_NONE);
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 4);

    gtk_widget_show_all(ui.win);

    g_timeout_add(500, tick_cb, NULL);

    GThread *t = g_thread_new("auth", auth_thread, NULL);
    g_thread_unref(t);

    gtk_main();
    return 0;
}