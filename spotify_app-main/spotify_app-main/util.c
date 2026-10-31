#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>

#define MAX_RESPONSE (4 * 1024 * 1024)

void buffer_init(Buffer *b) {
    b->len = 0;
    b->data = malloc(1);
    if (b->data) b->data[0] = '\0';
}

void buffer_free(Buffer *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
}

size_t receive_body(char *ptr, size_t size, size_t nmemb, void *userdata) {
    Buffer *b = userdata;
    size_t n = size * nmemb;

    if (!b->data || b->len + n > MAX_RESPONSE) return 0;

    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0;

    b->data = p;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

void base64url(const unsigned char *input, int len, char *output) {
    int n = EVP_EncodeBlock((unsigned char *)output, input, len);
    output[n] = '\0';

    for (int i = 0; i < n; i++) {
        if (output[i] == '+') output[i] = '-';
        if (output[i] == '/') output[i] = '_';
    }

    while (n > 0 && output[n - 1] == '=') {
        output[--n] = '\0';
    }
}

char *read_file(const char *name) {
    FILE *f = fopen(name, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0 || size > 1000000) { fclose(f); return NULL; }
    rewind(f);

    char *s = malloc((size_t)size + 1);
    if (!s) { fclose(f); return NULL; }

    size_t n = fread(s, 1, (size_t)size, f);
    fclose(f);
    s[n] = '\0';

    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';

    return s;
}

void url_decode(const char *src, char *dst, size_t cap) {
    size_t j = 0;

    for (size_t i = 0; src[i] && j + 1 < cap; i++) {
        if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char hex[3] = {src[i + 1], src[i + 2], '\0'};
            char *end;
            long v = strtol(hex, &end, 16);

            if (*end == '\0') {
                dst[j++] = (char)v;
                i += 2;
            } else {
                dst[j++] = src[i];
            }
        } else if (src[i] == '+') {
            dst[j++] = ' ';
        } else {
            dst[j++] = src[i];
        }
    }

    dst[j] = '\0';
}

int get_param(const char *query, const char *name, char *out, size_t cap) {
    size_t name_len = strlen(name);
    const char *p = query;

    while (p && *p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);

        const char *eq = memchr(p, '=', (size_t)(end - p));

        if (eq && (size_t)(eq - p) == name_len &&
            memcmp(p, name, name_len) == 0) {
            size_t n = (size_t)(end - eq - 1);
            char *tmp = malloc(n + 1);
            if (!tmp) return 0;

            memcpy(tmp, eq + 1, n);
            tmp[n] = '\0';
            url_decode(tmp, out, cap);
            free(tmp);
            return 1;
        }

        p = *end ? end + 1 : end;
    }

    return 0;
}
