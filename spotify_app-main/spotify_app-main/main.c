#include <curl/curl.h>

#include "ui.h"

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Login, playlists and playback all happen inside the window. */
    int rc = ui_run();

    curl_global_cleanup();
    return rc;
}
