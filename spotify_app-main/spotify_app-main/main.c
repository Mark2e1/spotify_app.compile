#include <stdio.h>
#include <curl/curl.h>

#include "auth.h"
#include "player.h"

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (auth_init() != 0) {
        fprintf(stderr, "Could not log in to Spotify.\n");
        curl_global_cleanup();
        return 1;
    }

    printf("\nLogged in. Make sure Spotify is open on one of your devices.\n");

    char line[64];
    for (;;) {
        printf("\n[p]ause  [b]ack [r]esume  [n]ext  [c]urrent [l]ist playlists [o]pen playlist [q]uit > ");
        fflush(stdout);

        if (!fgets(line, sizeof line, stdin)) break;

        char cmd = line[0];
        if (cmd == 'q') break;

        switch (cmd) {
        case 'p': player_pause();   break;
        case 'r': player_play();    break;
        case 'n': player_next();    break;
        case 'c': player_current(); break;
	case 'b': player_previous(); break;
	case 'l': player_list_playlists(); break;
    	case 'o': player_open_playlist();  break;

        case '\n': break;
        default:  printf("Unknown command.\n");
        }
    }

    curl_global_cleanup();
    return 0;
}
