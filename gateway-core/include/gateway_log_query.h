#ifndef GATEWAY_LOG_QUERY_H
#define GATEWAY_LOG_QUERY_H
#include "mongoose.h"
#include <errno.h>
#include <stdlib.h>

/* Mongoose: -4 means missing name; -1 means empty query/source.
 * Neither is an error for optional filters. Keep decoding/size errors invalid. */
static inline int log_query_text(struct mg_http_message *hm, const char *key, char *text, size_t size)
{
    int len = mg_http_get_var(&hm->query, key, text, size);
    return len >= 0 || len == -4 || len == -1;
}

static inline int log_query_number(struct mg_http_message *hm, const char *key, long long fallback, long long maximum, long long *out)
{
    char text[32];
    int len = mg_http_get_var(&hm->query, key, text, sizeof(text));
    if (len == -4 || len == -1 || len == 0) { *out = fallback; return 1; }
    if (len < 0) return 0;
    char *end = NULL; errno = 0;
    long long value = strtoll(text, &end, 10);
    if (errno || end == text || *end || value < 0 || value > maximum) return 0;
    *out = value; return 1;
}
#endif
