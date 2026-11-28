#ifndef GATEWAY_LOG_H
#define GATEWAY_LOG_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    int64_t id, timestamp, batch;
    int short_addr;
    char level[8], source[16], event[48], mac[32], message[1024];
} GatewayLog;
typedef struct {
    const char *level, *source, *mac, *search;
    int64_t from, to;
    int limit, offset;
    int current_run_only;
} GatewayLogFilter;
int gateway_log_init(const char *path);
void gateway_log_event(const char *level, const char *source, const char *event,
                       const char *mac, int short_addr, int64_t batch, const char *format, ...);
int gateway_log_flush(void);
void gateway_log_close(void);
int gateway_log_query(const GatewayLogFilter *filter, GatewayLog *rows, int *total);
uint64_t gateway_log_dropped(void);
#endif
