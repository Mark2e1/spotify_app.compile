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

#endif
