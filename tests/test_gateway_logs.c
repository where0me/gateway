#include <assert.h>
#include "../gateway-core/src/gateway_log.c"
#define TEST_LOG_DB "out/vibration-tests/gateway-logs-test.db"
static void drain(void) { int n; do { n = gateway_log_flush(); assert(n >= 0); } while (n > 0); }
static void *producer(void *arg) {
    for (int i = 0; i < 100; ++i) gateway_log_event("INFO", "node", "thread", "test", -1, i, "thread %lld item %d", (long long)(intptr_t)arg, i);
    return NULL;
}
int main(void) {
    remove(TEST_LOG_DB);
    assert(gateway_log_init(TEST_LOG_DB) == 0);
    gateway_log_event("INFO", "frontend", "sampling_saved", "AA:BB", 0x1234, 4294967295LL, "中文 'quote' 100%%");
    gateway_log_event("ERROR", "node", "failure", "CC:DD", -1, -1, "失败\n下一行");
    assert(sqlite3_exec(log_db, "CREATE TRIGGER simulate_disk_failure BEFORE INSERT ON logs BEGIN SELECT RAISE(ABORT,'test failure'); END;", NULL, NULL, NULL) == SQLITE_OK);
    assert(gateway_log_flush() == -1 && queued == 2);
    assert(sqlite3_exec(log_db, "DROP TRIGGER simulate_disk_failure;", NULL, NULL, NULL) == SQLITE_OK);
    drain();
    GatewayLog rows[1000]; int total;
    GatewayLogFilter f = {"", "", "", "", 0, 0, 200, 0, 0};
    assert(gateway_log_query(&f, rows, &total) == 2 && total == 2);
    assert(rows[0].short_addr == -1 && rows[0].batch == -1);
    assert(rows[1].batch == 4294967295LL && rows[1].short_addr == 0x1234);
    f.level = "ERROR"; assert(gateway_log_query(&f, rows, &total) == 1);
    f.level = ""; f.mac = "aa:bb"; assert(gateway_log_query(&f, rows, &total) == 1);
    f.mac = ""; f.search = "'quote'"; assert(gateway_log_query(&f, rows, &total) == 1);
    f.search = "' OR 1=1 --"; assert(gateway_log_query(&f, rows, &total) == 0);
    f.search = ""; f.offset = 1; assert(gateway_log_query(&f, rows, &total) == 1 && total == 2);
    f.offset = 0; f.from = time(NULL) + 60; assert(gateway_log_query(&f, rows, &total) == 0); f.from = 0;
    gateway_log_close(); assert(gateway_log_init(TEST_LOG_DB) == 0);
    assert(gateway_log_query(&f, rows, &total) == 2);
    f.current_run_only = 1;
    assert(gateway_log_query(&f, rows, &total) == 0 && total == 0);
    gateway_log_event("INFO", "backend", "system_start", NULL, -1, -1, "new run");
    drain();
    assert(gateway_log_query(&f, rows, &total) == 1 && total == 1);
    assert(strcmp(rows[0].event, "system_start") == 0);
    f.current_run_only = 0;
    pthread_t threads[4];
    for (intptr_t i = 0; i < 4; ++i) assert(pthread_create(&threads[i], NULL, producer, (void *)i) == 0);
    for (int i = 0; i < 4; ++i) pthread_join(threads[i], NULL);
    drain(); assert(gateway_log_query(&f, rows, &total) == 200 && total == 403);
    for (int i = 0; i < 600; ++i) gateway_log_event("INFO", "backend", "overflow", NULL, -1, -1, "overflow");
    assert(gateway_log_dropped() == 88); drain();
    assert(sqlite3_exec(log_db, "INSERT INTO logs(timestamp,level,source,event,mac,message) VALUES(1,'INFO','backend','old','','old');", NULL, NULL, NULL) == SQLITE_OK);
    last_cleanup = 0; drain(); f.search = "old"; assert(gateway_log_query(&f, rows, &total) == 0);
    f.search = "";
    f.limit = 0; assert(gateway_log_query(&f, rows, &total) == -1); f.limit = 200;
    assert(sqlite3_exec(log_db, "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<51000) INSERT INTO logs(timestamp,level,source,event,mac,message) SELECT strftime('%s','now'),'INFO','backend','capacity','','capacity' FROM n;", NULL, NULL, NULL) == SQLITE_OK);
    last_cleanup = 0; drain(); assert(gateway_log_query(&f, rows, &total) == 200 && total == 50000);
    for (int limit = 100; limit <= 1000; limit = limit == 100 ? 200 : limit == 200 ? 500 : 1000) {
        f.limit = limit;
        assert(gateway_log_query(&f, rows, &total) == limit && total == 50000);
        if (limit == 1000) break;
    }
    f.limit = 1001; assert(gateway_log_query(&f, rows, &total) == -1);
    gateway_log_close(); assert(remove(TEST_LOG_DB) == 0);
    puts("Gateway logs: filters, safe SQL, pagination, restart, concurrent queue, overflow, 7-day and 50k retention PASS");
    return 0;
}
