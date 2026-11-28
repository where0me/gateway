#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gateway_log_query.h"

static void valid_query(const char *query, const char *expected_source)
{
    struct mg_http_message hm = {0}; hm.query = mg_str(query);
    char level[16], source[24], mac[32], text[256];
    assert(log_query_text(&hm, "level", level, sizeof(level)));
    assert(log_query_text(&hm, "source", source, sizeof(source)));
    assert(log_query_text(&hm, "mac", mac, sizeof(mac)));
    assert(log_query_text(&hm, "q", text, sizeof(text)));
    assert(strcmp(source, expected_source) == 0);
    long long page, limit, from, to;
    assert(log_query_number(&hm, "page", 1, 100000, &page) && page == 1);
    assert(log_query_number(&hm, "limit", 50, 200, &limit) && limit == 50);
    assert(log_query_number(&hm, "from", 0, 4102444800LL, &from) && from == 0);
    assert(log_query_number(&hm, "to", 0, 4102444800LL, &to) && to == 0);
}
int main(void)
{
    valid_query("", "");
    valid_query("page=1&limit=50", "");
    valid_query("page=1&limit=50&source=backend", "backend");
    valid_query("page=1&limit=50&level=&mac=&from=&to=&q=", "");
    struct mg_http_message hm = {0}; char text[16]; long long value;
    hm.query = mg_str("q=%ZZ&page=NaN");
    assert(!log_query_text(&hm, "q", text, sizeof(text)));
    assert(!log_query_number(&hm, "page", 1, 100000, &value));
    hm.query = mg_str("q=too-long-for-buffer&limit=201");
    assert(!log_query_text(&hm, "q", text, 3));
    assert(!log_query_number(&hm, "limit", 50, 200, &value));
    hm.query = mg_str("from=-1");
    assert(!log_query_number(&hm, "from", 0, 4102444800LL, &value));
    puts("Log request parsing: actual Mongoose missing/empty filters, backend source, invalid URL/numbers/limits PASS");
    return 0;
}
