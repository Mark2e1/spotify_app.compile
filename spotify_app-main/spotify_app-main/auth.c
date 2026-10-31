#include "auth.h"
#include "config.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
typedef SOCKET sock_t;
typedef WSAPOLLFD pollfd_t;
#define SOCK_BAD INVALID_SOCKET
#define CLOSESOCK closesocket
#define SEND_FLAGS 0
#else
#include <poll.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
typedef int sock_t;
typedef struct pollfd pollfd_t;
#define SOCK_BAD (-1)
#define CLOSESOCK close
#define SEND_FLAGS MSG_NOSIGNAL
#endif

#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define TOKEN_URL "https://accounts.spotify.com/api/token"

static char g_access[1024];
static char g_refresh[1024];

const char *auth_access_token(void) {
    return g_access;
}

/* URL-escapes s. Free the result with curl_free(). */
static char *esc(const char *s) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    char *r = curl_easy_escape(c, s, 0);
    curl_easy_cleanup(c);
    return r;
}

/* ---------- token storage ---------- */

/* Windows: %APPDATA%\SpotifyApp\spotify_token.json (works no matter where the
   app is launched from, e.g. a desktop icon). Elsewhere: current folder. */
static const char *token_path(void) {
    static char path[1024];
    if (path[0]) return path;

#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata && *appdata) {
        char dir[900];
        snprintf(dir, sizeof dir, "%s\\SpotifyApp", appdata);
        CreateDirectoryA(dir, NULL);
        snprintf(path, sizeof path, "%s\\%s", dir, TOKEN_FILE);
        return path;
    }
#endif
    snprintf(path, sizeof path, "%s", TOKEN_FILE);
    return path;
}

static int save_tokens(void) {
    cJSON *json = cJSON_CreateObject();
    if (!json) return 1;

    cJSON_AddStringToObject(json, "access_token", g_access);
    cJSON_AddStringToObject(json, "refresh_token", g_refresh);

    char *text = cJSON_Print(json);
    cJSON_Delete(json);
    if (!text) return 1;

    FILE *f = fopen(token_path(), "w");
    if (!f) {
        perror("Could not save token file");
        free(text);
        return 1;
    }
    fputs(text, f);
    fclose(f);
    free(text);

#ifndef _WIN32
    chmod(token_path(), 0600); /* owner-only: it contains credentials */
#endif
    return 0;
}

static int load_tokens(void) {
    char *text = read_file(token_path());
    if (!text) return 1;

    cJSON *json = cJSON_Parse(text);
    free(text);
    if (!json) return 1;

    cJSON *a = cJSON_GetObjectItemCaseSensitive(json, "access_token");
    cJSON *r = cJSON_GetObjectItemCaseSensitive(json, "refresh_token");

    int ok = cJSON_IsString(r) && r->valuestring &&
             strlen(r->valuestring) < sizeof g_refresh;

    if (ok) {
        snprintf(g_refresh, sizeof g_refresh, "%s", r->valuestring);
        if (cJSON_IsString(a) && a->valuestring &&
            strlen(a->valuestring) < sizeof g_access)
            snprintf(g_access, sizeof g_access, "%s", a->valuestring);
    }

    cJSON_Delete(json);
    return ok ? 0 : 1;
}

/* ---------- talking to the token endpoint ---------- */

/* POSTs a form body; returns parsed JSON on HTTP 200, else NULL. */
static cJSON *token_request(const char *post) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    Buffer body;
    buffer_init(&body);

    struct curl_slist *headers = curl_slist_append(
        NULL, "Content-Type: application/x-www-form-urlencoded");

    curl_easy_setopt(curl, CURLOPT_URL, TOKEN_URL);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
#ifdef _WIN32
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    cJSON *json = NULL;
    if (rc != CURLE_OK) {
        fprintf(stderr, "Token request failed: %s\n", curl_easy_strerror(rc));
    } else if (status != 200) {
        fprintf(stderr, "Spotify rejected the token request (HTTP %ld): %.300s\n",
                status, body.data ? body.data : "");
    } else if (body.data) {
        json = cJSON_Parse(body.data);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    buffer_free(&body);
    return json;
}

/* Copies tokens out of a token response and saves them. A refresh response
   may omit refresh_token; in that case the old one is kept. */
static int apply_token_response(cJSON *json) {
cJSON *scope = cJSON_GetObjectItemCaseSensitive(json, "scope");

if (cJSON_IsString(scope)) {
    printf("\nSpotify granted scopes: %s\n", scope->valuestring);
} else {
    printf("\nSpotify did not return a scope field.\n");
}
    cJSON *a = cJSON_GetObjectItemCaseSensitive(json, "access_token");
    cJSON *r = cJSON_GetObjectItemCaseSensitive(json, "refresh_token");

    if (!cJSON_IsString(a) || !a->valuestring ||
        strlen(a->valuestring) >= sizeof g_access) {
        fprintf(stderr, "Spotify response did not contain a usable access token.\n");
        return 1;
    }
    snprintf(g_access, sizeof g_access, "%s", a->valuestring);

    if (cJSON_IsString(r) && r->valuestring &&
        strlen(r->valuestring) < sizeof g_refresh)
        snprintf(g_refresh, sizeof g_refresh, "%s", r->valuestring);

    return save_tokens();
}

int auth_refresh(void) {
    if (!g_refresh[0]) return 1;

    char *er = esc(g_refresh);
    char *ei = esc(SPOTIFY_CLIENT_ID);
    if (!er || !ei) {
        if (er) curl_free(er);
        if (ei) curl_free(ei);
        return 1;
    }

    char post[2048];
    int n = snprintf(post, sizeof post,
                     "grant_type=refresh_token&refresh_token=%s&client_id=%s",
                     er, ei);
    curl_free(er);
    curl_free(ei);
    if (n < 0 || (size_t)n >= sizeof post) return 1;

    cJSON *json = token_request(post);
    if (!json) return 1;

    int rc = apply_token_response(json);
    cJSON_Delete(json);
    return rc;
}

/* ---------- browser login (PKCE) ---------- */

/* query = "code=...&state=..." (from the callback request or a pasted URL). */
static int finish_login(const char *query, const char *expected_state,
                        const char *verifier) {
    char code[2048] = {0};
    char state[256] = {0};
    char error[256] = {0};

    get_param(query, "code", code, sizeof code);
    get_param(query, "state", state, sizeof state);
    get_param(query, "error", error, sizeof error);

    if (error[0]) {
        fprintf(stderr, "Spotify login was denied: %s\n", error);
        return 1;
    }
    if (!state[0] || strcmp(state, expected_state) != 0) {
        fprintf(stderr, "State check failed. Restart the app and try again.\n");
        return 1;
    }
    if (!code[0]) {
        fprintf(stderr, "No authorization code in the redirect.\n");
        return 1;
    }

    char *ec = esc(code);
    char *ev = esc(verifier);
    char *er = esc(SPOTIFY_REDIRECT_URI);
    char *ei = esc(SPOTIFY_CLIENT_ID);

    int rc = 1;
    if (ec && ev && er && ei) {
        char post[4096];
        int n = snprintf(post, sizeof post,
                         "client_id=%s&grant_type=authorization_code"
                         "&code=%s&redirect_uri=%s&code_verifier=%s",
                         ei, ec, er, ev);
        if (n > 0 && (size_t)n < sizeof post) {
            cJSON *json = token_request(post);
            if (json) {
                rc = apply_token_response(json);
                cJSON_Delete(json);
            }
        }
    }

    if (ec) curl_free(ec);
    if (ev) curl_free(ev);
    if (er) curl_free(er);
    if (ei) curl_free(ei);
    return rc;
}

static sock_t open_listener(void) {
    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == SOCK_BAD) return SOCK_BAD;

#ifndef _WIN32
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(SPOTIFY_CALLBACK_PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(fd, 4) != 0) {
        CLOSESOCK(fd);
        return SOCK_BAD;
    }
    return fd;
}

/* Browsers sometimes open an idle "preconnect" socket first; don't let that
   block us forever. */
static void set_recv_timeout(sock_t s, int ms) {
#ifdef _WIN32
    DWORD tv = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

static void reply(sock_t client, const char *status_line, const char *message) {
    char body[512];
    snprintf(body, sizeof body,
             "<!doctype html><html><body><h2>Spotify app</h2><p>%s</p>"
             "<p>You can close this tab.</p></body></html>",
             message);

    char resp[1024];
    int n = snprintf(resp, sizeof resp,
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Connection: close\r\n"
                     "Content-Length: %lu\r\n\r\n%s",
                     status_line, (unsigned long)strlen(body), body);

    if (n > 0 && (size_t)n < sizeof resp)
        send(client, resp, n, SEND_FLAGS);
}

/* Waits for the browser callback on the local port. On Linux/macOS the user
   can alternatively paste the redirect URL into the terminal (needed in a
   browser-based Codespace, where the browser can't reach localhost). */
static int wait_for_login(sock_t server, const char *state, const char *verifier) {
#ifndef _WIN32
    int stdin_open = 1;
#endif

    for (;;) {
        pollfd_t fds[2];
        int nfds = 1;
        memset(fds, 0, sizeof fds);
        fds[0].fd = server;
        fds[0].events = POLLIN;
#ifndef _WIN32
        fds[1].fd = stdin_open ? STDIN_FILENO : -1;
        fds[1].events = POLLIN;
        nfds = 2;
#endif

#ifdef _WIN32
        int pr = WSAPoll(fds, nfds, 5 * 60 * 1000);
#else
        int pr = poll(fds, nfds, 5 * 60 * 1000);
#endif
        if (pr == 0) {
            fprintf(stderr, "Timed out waiting for login.\n");
            return 1;
        }
        if (pr < 0) {
            fprintf(stderr, "Waiting for login failed.\n");
            return 1;
        }

        if (fds[0].revents & POLLIN) {
            sock_t client = accept(server, NULL, NULL);
            if (client == SOCK_BAD) continue;

            set_recv_timeout(client, 3000);

            char req[8192];
            int got = (int)recv(client, req, (int)(sizeof req - 1), 0);
            if (got <= 0) {
                CLOSESOCK(client);
                continue;
            }
            req[got] = '\0';

            const char *prefix = "GET /callback?";
            if (strncmp(req, prefix, strlen(prefix)) != 0) {
                /* e.g. a favicon request: ignore and keep waiting */
                reply(client, "404 Not Found", "Not found.");
                CLOSESOCK(client);
                continue;
            }

            char *query = req + strlen(prefix);
            char *end = strpbrk(query, " \r\n");
            if (end) *end = '\0';

            int rc = finish_login(query, state, verifier);
            reply(client, "200 OK",
                  rc == 0 ? "Spotify connected successfully!"
                          : "Login failed. Check the terminal.");
            CLOSESOCK(client);
            return rc;
        }

#ifndef _WIN32
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            char line[8192];
            ssize_t n = read(STDIN_FILENO, line, sizeof line - 1);
            if (n <= 0) {
                stdin_open = 0;
                continue;
            }
            line[n] = '\0';

            char *q = strchr(line, '?');
            if (!q) {
                fprintf(stderr,
                        "That doesn't look like the redirect URL (no '?'). "
                        "Paste the full address from the browser's address bar.\n");
                continue;
            }
            q++;
            char *end = strpbrk(q, " \t\r\n#");
            if (end) *end = '\0';

            return finish_login(q, state, verifier);
        }
#endif
    }
}

static int auth_login(void) {
    unsigned char rnd[32];
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    char verifier[100], challenge[100], state[100];

    if (RAND_bytes(rnd, sizeof rnd) != 1) {
        fprintf(stderr, "Could not generate secure random data.\n");
        return 1;
    }
    base64url(rnd, sizeof rnd, verifier);

    if (EVP_Digest(verifier, strlen(verifier), hash, &hash_len,
                   EVP_sha256(), NULL) != 1) {
        fprintf(stderr, "Could not generate PKCE challenge.\n");
        return 1;
    }
    base64url(hash, (int)hash_len, challenge);

    if (RAND_bytes(rnd, 16) != 1) {
        fprintf(stderr, "Could not generate state.\n");
        return 1;
    }
    base64url(rnd, 16, state);

    char *scopes = esc(SPOTIFY_SCOPES);
    printf("\nDEBUG: Requested Spotify scopes: %s\n", SPOTIFY_SCOPES);
    char *redir  = esc(SPOTIFY_REDIRECT_URI);
    char *cid    = esc(SPOTIFY_CLIENT_ID);
    if (!scopes || !redir || !cid) {
        if (scopes) curl_free(scopes);
        if (redir)  curl_free(redir);
        if (cid)    curl_free(cid);
        return 1;
    }

    char url[2048];
    int un = snprintf(url, sizeof url,
                  "https://accounts.spotify.com/authorize"
                  "?client_id=%s&response_type=code&redirect_uri=%s&scope=%s"
                  "&code_challenge_method=S256&code_challenge=%s&state=%s"
                  "&show_dialog=true",
                  cid, redir, scopes, challenge, state);
    curl_free(scopes);
    curl_free(redir);
    curl_free(cid);
    if (un < 0 || (size_t)un >= sizeof url) return 1;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "Winsock initialization failed.\n");
        return 1;
    }
#endif

    sock_t server = open_listener();

#ifdef _WIN32
    if (server == SOCK_BAD) {
        fprintf(stderr,
                "Could not listen on port %d. Is another copy of the app "
                "running?\n", SPOTIFY_CALLBACK_PORT);
        WSACleanup();
        return 1;
    }
#else
    if (server == SOCK_BAD)
        fprintf(stderr,
                "Note: could not listen on port %d. You can still log in by "
                "pasting the redirect URL below.\n", SPOTIFY_CALLBACK_PORT);
#endif

    printf("\nOpen this URL in your browser and approve access:\n\n%s\n\n", url);
#ifdef _WIN32
    printf("Opening your browser. Waiting for Spotify login to finish...\n\n");
    fflush(stdout);
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#else
    printf("Waiting for Spotify to redirect to http://127.0.0.1:%d/callback ...\n"
           "If the page fails to load, copy the full URL from your browser's\n"
           "address bar, paste it here and press Enter.\n\n",
           SPOTIFY_CALLBACK_PORT);
    fflush(stdout);
#endif

    int rc = wait_for_login(server, state, verifier);
    if (server != SOCK_BAD) CLOSESOCK(server);

#ifdef _WIN32
    WSACleanup();
#endif

    if (rc == 0) printf("Spotify login succeeded.\n");
    return rc;
}


int auth_init(void) {
    if (load_tokens() == 0) {
        if (auth_refresh() == 0) return 0;
        fprintf(stderr, "Saved login is no longer valid; logging in again.\n");
    }
    return auth_login();
}
