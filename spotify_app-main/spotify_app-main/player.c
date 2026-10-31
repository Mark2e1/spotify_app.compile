
#include "player.h"
#include "auth.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <cjson/cJSON.h>

#define API "https://api.spotify.com/v1/me/player"
#define PLAYLISTS_API "https://api.spotify.com/v1/me/playlists"

/* One HTTP request. Returns HTTP status or -1 on network error. */
static long api_once(const char *method, const char *url,
                     const char *payload, Buffer *out) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    char auth[1200];

    snprintf(auth, sizeof auth, "Authorization: Bearer %s",
             auth_access_token());

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth);

    if (payload != NULL) {
        headers = curl_slist_append(
            headers, "Content-Type: application/json"
        );
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

#ifdef _WIN32
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS,
                     (long)CURLSSLOPT_NATIVE_CA);
#endif

    if (strcmp(method, "GET") != 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);

        if (payload != NULL) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
        } else {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        }
    }

    long status = -1;

    CURLcode rc = curl_easy_perform(curl);

    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    } else {
        fprintf(stderr, "Network error: %s\n",
                curl_easy_strerror(rc));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return status;
}

/* Request with automatic token refresh on HTTP 401. */
static long api_call(const char *method, const char *url,
                     const char *payload, Buffer *out) {
    buffer_init(out);

    long status = api_once(method, url, payload, out);

    if (status == 401 && auth_refresh() == 0) {
        buffer_free(out);
        buffer_init(out);

        status = api_once(method, url, payload, out);
    }

    return status;
}

/* Prints helpful error messages. */
static int check(long status, const Buffer *b) {
    if (status >= 200 && status < 300)
        return 0;

    switch (status) {
        case -1:
            break;

        case 401:
            fprintf(stderr, "Not authorized. Log in again.\n");
            break;

        case 403:
    	    fprintf(stderr, "\n===== SPOTIFY ERROR =====\n");
    	    fprintf(stderr, "HTTP Status: %ld\n", status);
            fprintf(stderr, "Response: %s\n",
            b->data ? b->data : "Empty response");
            fprintf(stderr, "=========================\n");
            break;

        case 404:
    	    fprintf(stderr,
            "Spotify resource not found.\n"
            "Check the endpoint or resource ID.\n");
    	     break;

        case 429:
            fprintf(stderr,
                    "Rate limited by Spotify. Try again later.\n");
            break;

        default:
            fprintf(stderr, "Spotify HTTP %ld: %.200s\n",
                    status, b->data ? b->data : "");
    }

    return 1;
}

/* Generic playback command. */
static int simple_command(const char *method,
                          const char *path,
                          const char *message) {
    char url[256];

    snprintf(url, sizeof url, API "%s", path);

    Buffer b;

    long status = api_call(method, url, NULL, &b);

    int rc = check(status, &b);

    buffer_free(&b);

    if (rc == 0)
        printf("%s\n", message);

    return rc;
}

/* Playback commands. */
int player_pause(void) {
    return simple_command("PUT", "/pause", "Paused.");
}

int player_play(void) {
    return simple_command("PUT", "/play", "Playing.");
}

int player_next(void) {
    return simple_command("POST", "/next",
                          "Skipped to next track.");
}

int player_previous(void) {
    return simple_command("POST", "/previous",
                          "Previous track.");
}

/* Convert milliseconds into minutes and seconds. */
static void print_time(const char *label, double ms) {
    long total = (long)(ms / 1000.0);

    printf("%s%ld:%02ld",
           label, total / 60, total % 60);
}



/* Currently playing information, including playlist name. */
int player_current(void) {
    Buffer b;

    long status = api_call(
        "GET", API "/currently-playing", NULL, &b
    );

    if (status == 204) {
        printf("Nothing is playing right now.\n");
        buffer_free(&b);
        return 0;
    }

    if (check(status, &b)) {
        buffer_free(&b);
        return 1;
    }

    cJSON *json = b.data ? cJSON_Parse(b.data) : NULL;
    buffer_free(&b);

    if (!json) {
        printf("Could not read playback information.\n");
        return 1;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(
        json, "item"
    );

    if (!cJSON_IsObject(item)) {
        printf("Nothing is playing right now.\n");
        cJSON_Delete(json);
        return 0;
    }

    /* Get playlist name */
    cJSON *context = cJSON_GetObjectItemCaseSensitive(
        json, "context"
    );

    int playlist_found = 0;

    if (cJSON_IsObject(context)) {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(
            context, "type"
        );

        cJSON *uri = cJSON_GetObjectItemCaseSensitive(
            context, "uri"
        );

        if (cJSON_IsString(type) &&
            strcmp(type->valuestring, "playlist") == 0 &&
            cJSON_IsString(uri)) {

            const char *prefix = "spotify:playlist:";

            if (strncmp(uri->valuestring, prefix,
                        strlen(prefix)) == 0) {

                char playlist_url[512];

                snprintf(
                    playlist_url,
                    sizeof playlist_url,
                    "https://api.spotify.com/v1/playlists/%s",
                    uri->valuestring + strlen(prefix)
                );

                Buffer pb;

                long ps = api_call(
                    "GET", playlist_url, NULL, &pb
                );

                if (ps >= 200 && ps < 300) {
                    cJSON *pj = pb.data
                        ? cJSON_Parse(pb.data)
                        : NULL;

                    if (pj) {
                        cJSON *pn =
                            cJSON_GetObjectItemCaseSensitive(
                                pj, "name"
                            );

                        if (cJSON_IsString(pn)) {
                            printf("Playlist : %s\n",
                                   pn->valuestring);
                            playlist_found = 1;
                        }

                        cJSON_Delete(pj);
                    }
                }

                buffer_free(&pb);
            }
        }
    }

    if (!playlist_found) {
        printf("Playlist : Not available (possibly an album, artist, or single track)\n");
    }

    /* Track */
    cJSON *name = cJSON_GetObjectItemCaseSensitive(
        item, "name"
    );

    printf("Track    : %s\n",
           cJSON_IsString(name) ? name->valuestring : "?");

    /* Artists */
    cJSON *artists = cJSON_GetObjectItemCaseSensitive(
        item, "artists"
    );

    if (cJSON_IsArray(artists)) {
        printf("Artist   : ");

        int first = 1;
        cJSON *artist;

        cJSON_ArrayForEach(artist, artists) {
            cJSON *an = cJSON_GetObjectItemCaseSensitive(
                artist, "name"
            );

            if (cJSON_IsString(an)) {
                printf("%s%s",
                       first ? "" : ", ",
                       an->valuestring);
                first = 0;
            }
        }

        printf("\n");
    }

    /* Playback status */
    cJSON *playing = cJSON_GetObjectItemCaseSensitive(
        json, "is_playing"
    );

    cJSON *progress = cJSON_GetObjectItemCaseSensitive(
        json, "progress_ms"
    );

    cJSON *duration = cJSON_GetObjectItemCaseSensitive(
        item, "duration_ms"
    );

    printf("Status   : %s\n",
           cJSON_IsTrue(playing) ? "playing" : "paused");

    if (cJSON_IsNumber(progress) && cJSON_IsNumber(duration)) {
        print_time("Time     : ", progress->valuedouble);
        print_time(" / ", duration->valuedouble);
        printf("\n");
    }

    cJSON_Delete(json);
    return 0;
}
/* List Spotify playlists. */
int player_list_playlists(void) {
    Buffer b;

    long status = api_call(
        "GET", PLAYLISTS_API, NULL, &b
    );

    if (check(status, &b)) {
        buffer_free(&b);
        return 1;
    }

    cJSON *json = b.data ? cJSON_Parse(b.data) : NULL;
    buffer_free(&b);

    if (!json) {
        printf("Could not parse playlists.\n");
        return 1;
    }

    cJSON *items = cJSON_GetObjectItemCaseSensitive(
        json, "items"
    );

    if (!cJSON_IsArray(items) ||
        cJSON_GetArraySize(items) == 0) {

        printf("No playlists found.\n");
        cJSON_Delete(json);
        return 0;
    }

    printf("\n===== YOUR SPOTIFY PLAYLISTS =====\n");

    cJSON *playlist;
    int i = 1;

    cJSON_ArrayForEach(playlist, items) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(
            playlist, "name"
        );

        printf("%d. %s\n",
               i++,
               cJSON_IsString(name)
                   ? name->valuestring
                   : "Unknown");
    }

    cJSON_Delete(json);
    return 0;
}

/*
 * Open and play a selected playlist.
 * This version retrieves playlist details and prepares
 * the selection. See note below about playback support.
 */
int player_open_playlist(void) {
    Buffer b;

    long status = api_call(
        "GET", PLAYLISTS_API, NULL, &b
    );

    if (check(status, &b)) {
        buffer_free(&b);
        return 1;
    }

    cJSON *json = b.data ? cJSON_Parse(b.data) : NULL;
    buffer_free(&b);

    if (!json) {
        printf("Could not parse playlists.\n");
        return 1;
    }

    cJSON *items = cJSON_GetObjectItemCaseSensitive(
        json, "items"
    );

    if (!cJSON_IsArray(items) ||
        cJSON_GetArraySize(items) == 0) {

        printf("No playlists found.\n");
        cJSON_Delete(json);
        return 0;
    }

    int count = cJSON_GetArraySize(items);

    printf("\n===== SELECT A PLAYLIST =====\n");

    for (int i = 0; i < count; i++) {
        cJSON *playlist = cJSON_GetArrayItem(items, i);

        cJSON *name = cJSON_GetObjectItemCaseSensitive(
            playlist, "name"
        );

        printf("%d. %s\n",
               i + 1,
               cJSON_IsString(name)
                   ? name->valuestring
                   : "Unknown");
    }

    int choice;

    printf("\nEnter playlist number: ");

    if (scanf("%d", &choice) != 1) {
        printf("Invalid input.\n");

        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF) {}

        cJSON_Delete(json);
        return 1;
    }

    if (choice < 1 || choice > count) {
        printf("Invalid playlist number.\n");
        cJSON_Delete(json);
        return 1;
    }

    cJSON *selected = cJSON_GetArrayItem(items, choice - 1);

    cJSON *name = cJSON_GetObjectItemCaseSensitive(
        selected, "name"
    );

    cJSON *id = cJSON_GetObjectItemCaseSensitive(
        selected, "id"
    );

    if (!cJSON_IsString(id)) {
        printf("Playlist ID unavailable.\n");
        cJSON_Delete(json);
        return 1;
    }

    char playlist_id[256];

    snprintf(playlist_id, sizeof playlist_id,
             "%s", id->valuestring);

    printf("\nSelected playlist: %s\n",
           cJSON_IsString(name) ? name->valuestring : "Unknown");

    cJSON_Delete(json);

    char url[512];
    snprintf(url, sizeof url, API "/play");

    cJSON *body = cJSON_CreateObject();

    char uri[320];

    snprintf(uri, sizeof uri,
             "spotify:playlist:%s", playlist_id);

    cJSON_AddStringToObject(body, "context_uri", uri);

    char *payload = cJSON_PrintUnformatted(body);

    if (!payload) {
        cJSON_Delete(body);
        return 1;
    }

    Buffer response;

    status = api_call("PUT", url, payload, &response);

    int rc = check(status, &response);

    if (rc == 0)
        printf("Playlist playback requested.\n");

    buffer_free(&response);
    free(payload);
    cJSON_Delete(body);

    return rc;
}

/* ================= GUI support: no printing, returns HTTP status ================= */

long player_http_cmd(const char *method, const char *path) {
    char url[512];
    snprintf(url, sizeof url, API "%s", path);

    Buffer b;
    long status = api_call(method, url, NULL, &b);
    buffer_free(&b);
    return status;
}

long player_play_playlist_id(const char *id) {
    char uri[320];
    snprintf(uri, sizeof uri, "spotify:playlist:%s", id);

    cJSON *body = cJSON_CreateObject();
    if (!body) return -1;
    cJSON_AddStringToObject(body, "context_uri", uri);
    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!payload) return -1;

    Buffer b;
    long status = api_call("PUT", API "/play", payload, &b);
    buffer_free(&b);
    free(payload);
    return status;
}

long player_get_state(PlayerState *st) {
    memset(st, 0, sizeof *st);
    st->volume = -1;

    Buffer b;
    long status = api_call("GET", API, NULL, &b);
    if (status != 200) {
        buffer_free(&b);
        return status;
    }

    cJSON *json = b.data ? cJSON_Parse(b.data) : NULL;
    buffer_free(&b);
    if (!json) return -1;

    cJSON *device = cJSON_GetObjectItemCaseSensitive(json, "device");
    if (cJSON_IsObject(device)) {
        cJSON *vol = cJSON_GetObjectItemCaseSensitive(device, "volume_percent");
        if (cJSON_IsNumber(vol)) st->volume = (int)vol->valuedouble;
    }

    st->playing = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "is_playing"));
    st->shuffle = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "shuffle_state"));

    cJSON *ctx = cJSON_GetObjectItemCaseSensitive(json, "context");
    if (cJSON_IsObject(ctx)) {
        cJSON *ctype = cJSON_GetObjectItemCaseSensitive(ctx, "type");
        cJSON *curi  = cJSON_GetObjectItemCaseSensitive(ctx, "uri");
        const char *prefix = "spotify:playlist:";

        if (cJSON_IsString(ctype) && ctype->valuestring &&
            strcmp(ctype->valuestring, "playlist") == 0 &&
            cJSON_IsString(curi) && curi->valuestring &&
            strncmp(curi->valuestring, prefix, strlen(prefix)) == 0)
            snprintf(st->context_id, sizeof st->context_id, "%s",
                     curi->valuestring + strlen(prefix));
    }

    cJSON *progress = cJSON_GetObjectItemCaseSensitive(json, "progress_ms");
    if (cJSON_IsNumber(progress)) st->progress_ms = (long)progress->valuedouble;

    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, "item");
    if (cJSON_IsObject(item)) {
        st->active = 1;

        cJSON *turi = cJSON_GetObjectItemCaseSensitive(item, "uri");
        if (cJSON_IsString(turi) && turi->valuestring)
            snprintf(st->track_uri, sizeof st->track_uri, "%s", turi->valuestring);

        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (cJSON_IsString(name) && name->valuestring)
            snprintf(st->track, sizeof st->track, "%s", name->valuestring);

        cJSON *dur = cJSON_GetObjectItemCaseSensitive(item, "duration_ms");
        if (cJSON_IsNumber(dur)) st->duration_ms = (long)dur->valuedouble;

        cJSON *artists = cJSON_GetObjectItemCaseSensitive(item, "artists");
        if (cJSON_IsArray(artists)) {
            size_t len = 0;
            cJSON *a;
            cJSON_ArrayForEach(a, artists) {
                cJSON *an = cJSON_GetObjectItemCaseSensitive(a, "name");
                if (!cJSON_IsString(an) || !an->valuestring) continue;

                int n = snprintf(st->artist + len, sizeof st->artist - len,
                                 "%s%s", len ? ", " : "", an->valuestring);
                if (n < 0 || (size_t)n >= sizeof st->artist - len) break;
                len += (size_t)n;
            }
        }
    }

    cJSON_Delete(json);
    return 200;
}

long player_fetch_playlists(PlaylistInfo **out, int *count) {
    *out = NULL;
    *count = 0;

    Buffer b;
    long status = api_call("GET", PLAYLISTS_API "?limit=50", NULL, &b);
    if (status != 200) {
        buffer_free(&b);
        return status;
    }

    cJSON *json = b.data ? cJSON_Parse(b.data) : NULL;
    buffer_free(&b);
    if (!json) return -1;

    cJSON *items = cJSON_GetObjectItemCaseSensitive(json, "items");
    int n = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;

    if (n > 0) {
        PlaylistInfo *list = calloc((size_t)n, sizeof *list);
        if (!list) {
            cJSON_Delete(json);
            return -1;
        }

        int k = 0;
        cJSON *it;
        cJSON_ArrayForEach(it, items) {
            cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "name");
            cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
            if (!cJSON_IsString(nm) || !nm->valuestring ||
                !cJSON_IsString(id) || !id->valuestring)
                continue;

            snprintf(list[k].name, sizeof list[k].name, "%s", nm->valuestring);
            snprintf(list[k].id, sizeof list[k].id, "%s", id->valuestring);
            k++;
        }

        *out = list;
        *count = k;
    }

    cJSON_Delete(json);
    return 200;
}

long player_fetch_playlist_tracks(const char *playlist_id, TrackInfo **out, int *count) {
    *out = NULL;
    *count = 0;

    TrackInfo *list = NULL;
    int n = 0, cap = 0;

    for (int offset = 0; offset < 1000; offset += 100) {
        char url[512];
        snprintf(url, sizeof url,
                 "https://api.spotify.com/v1/playlists/%s/items?limit=100&offset=%d",
                 playlist_id, offset);

        Buffer b;
        long status = api_call("GET", url, NULL, &b);
        if (status != 200) {
            buffer_free(&b);
            if (n == 0) {
                free(list);
                return status;
            }
            break;
        }

        cJSON *json = b.data ? cJSON_Parse(b.data) : NULL;
        buffer_free(&b);
        if (!json) break;

        cJSON *items = cJSON_GetObjectItemCaseSensitive(json, "items");
        int page = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
        if (page == 0) {
            cJSON_Delete(json);
            break;
        }

        int oom = 0;
        cJSON *it;
        cJSON_ArrayForEach(it, items) {
            /* Spotify's newer API calls it "item", the older one "track". */
            cJSON *tr = cJSON_GetObjectItemCaseSensitive(it, "item");
            if (!cJSON_IsObject(tr)) tr = cJSON_GetObjectItemCaseSensitive(it, "track");
            if (!cJSON_IsObject(tr)) continue;

            cJSON *nm  = cJSON_GetObjectItemCaseSensitive(tr, "name");
            cJSON *uri = cJSON_GetObjectItemCaseSensitive(tr, "uri");
            if (!cJSON_IsString(nm) || !nm->valuestring ||
                !cJSON_IsString(uri) || !uri->valuestring)
                continue;

            if (n == cap) {
                int ncap = cap ? cap * 2 : 128;
                TrackInfo *grown = realloc(list, (size_t)ncap * sizeof *list);
                if (!grown) { oom = 1; break; }
                list = grown;
                cap = ncap;
            }

            TrackInfo *ti = &list[n++];
            memset(ti, 0, sizeof *ti);
            snprintf(ti->name, sizeof ti->name, "%s", nm->valuestring);
            snprintf(ti->uri, sizeof ti->uri, "%s", uri->valuestring);

            cJSON *artists = cJSON_GetObjectItemCaseSensitive(tr, "artists");
            if (cJSON_IsArray(artists)) {
                size_t len = 0;
                cJSON *a;
                cJSON_ArrayForEach(a, artists) {
                    cJSON *an = cJSON_GetObjectItemCaseSensitive(a, "name");
                    if (!cJSON_IsString(an) || !an->valuestring) continue;

                    int w = snprintf(ti->artist + len, sizeof ti->artist - len,
                                     "%s%s", len ? ", " : "", an->valuestring);
                    if (w < 0 || (size_t)w >= sizeof ti->artist - len) break;
                    len += (size_t)w;
                }
            }
        }

        cJSON_Delete(json);
        if (oom || page < 100) break;
    }

    *out = list;
    *count = n;
    return 200;
}

long player_play_track_in_playlist(const char *playlist_id, const char *track_uri) {
    char ctx[320];
    snprintf(ctx, sizeof ctx, "spotify:playlist:%s", playlist_id);

    cJSON *body = cJSON_CreateObject();
    if (!body) return -1;
    cJSON_AddStringToObject(body, "context_uri", ctx);
    cJSON *offset = cJSON_AddObjectToObject(body, "offset");
    if (offset) cJSON_AddStringToObject(offset, "uri", track_uri);

    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!payload) return -1;

    Buffer b;
    long status = api_call("PUT", API "/play", payload, &b);
    buffer_free(&b);
    free(payload);
    return status;
}
