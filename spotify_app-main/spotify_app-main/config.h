#ifndef CONFIG_H
#define CONFIG_H

#define SPOTIFY_CLIENT_ID "55b4ff4c4cef4e279077056d3294053c"

#define SPOTIFY_REDIRECT_URI "http://127.0.0.1:8888/callback"

#define SPOTIFY_CALLBACK_PORT 8888

#define SPOTIFY_SCOPES \
    "user-read-currently-playing " \
    "user-read-playback-state " \
    "user-modify-playback-state " \
    "playlist-read-private " \
    "playlist-read-collaborative"

#define TOKEN_FILE "spotify_token.json"

#endif