#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

/* Growable buffer for HTTP responses. */
typedef struct {
    char  *data;
    size_t len;
} Buffer;

void   buffer_init(Buffer *b);
void   buffer_free(Buffer *b);
size_t receive_body(char *ptr, size_t size, size_t nmemb, void *userdata);

/* output must hold at least 4 * ((len + 2) / 3) + 1 bytes. */
void  base64url(const unsigned char *input, int len, char *output);

/* Reads a whole file, strips trailing newlines. Caller frees. NULL on error. */
char *read_file(const char *name);

void  url_decode(const char *src, char *dst, size_t cap);

/* Finds name=value in a query string. Returns 1 if found. */
int   get_param(const char *query, const char *name, char *out, size_t cap);

#endif
