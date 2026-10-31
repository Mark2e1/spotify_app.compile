#ifndef PLAYER_H
#define PLAYER_H

/* Each returns 0 on success, nonzero on failure (message printed). */
int player_pause(void);
int player_play(void);
int player_next(void);
int player_current(void);
int player_previous(void);
int player_list_playlists(void);
int player_open_playlist(void);

/* ---- Used by the GTK window. These print nothing and return the HTTP
   status (200-299 = success, -1 = network error). ---- */
typedef struct {
    int  active;        /* a track is loaded */
    int  playing;
    int  shuffle;
    int  volume;        /* 0-100, or -1 if the device has no volume control */
    char track[256];
    char artist[256];
    char track_uri[96];
    char context_id[64];   /* id of the playlist that is playing, "" if none */
    long progress_ms;
    long duration_ms;
} PlayerState;

typedef struct {
    char name[256];
    char id[64];
} PlaylistInfo;

long player_get_state(PlayerState *st);               /* 200 ok, 204 no device */
long player_fetch_playlists(PlaylistInfo **out, int *count); /* caller free()s *out */
long player_play_playlist_id(const char *id);
long player_http_cmd(const char *method, const char *path);  /* e.g. "PUT", "/pause" */

typedef struct {
    char name[256];
    char artist[160];
    char uri[96];
} TrackInfo;

/* Songs of a playlist (up to 1000). *out may be NULL with count 0 if Spotify
   hides the songs (it only allows this for playlists you own). */
long player_fetch_playlist_tracks(const char *playlist_id, TrackInfo **out, int *count);
long player_play_track_in_playlist(const char *playlist_id, const char *track_uri);

#endif
