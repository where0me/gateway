#include "gateway_log.h"
#include <pthread.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Producers only enqueue. The HTTP/main thread owns the independent log DB. */
#define LOG_QUEUE_SIZE 512
#define LOG_BATCH_SIZE 32
#define LOG_MAX_ROWS 50000
#define LOG_KEEP_SECONDS (7LL * 86400)
static GatewayLog queue[LOG_QUEUE_SIZE];
static unsigned head, queued;
static uint64_t dropped;
static int enabled;
static sqlite3 *log_db;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static time_t last_cleanup;
static int64_t run_start_id;

int gateway_log_init(const char *path)
{
    if (sqlite3_open(path, &log_db) != SQLITE_OK) goto fail;
    sqlite3_busy_timeout(log_db, 50);
    if (sqlite3_exec(log_db,
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
        "CREATE TABLE IF NOT EXISTS logs(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "timestamp INTEGER NOT NULL,level TEXT NOT NULL,source TEXT NOT NULL,"
        "event TEXT NOT NULL,mac TEXT NOT NULL,short_addr INTEGER,batch INTEGER,message TEXT NOT NULL);"
        "CREATE INDEX IF NOT EXISTS logs_time ON logs(timestamp);"
        "CREATE INDEX IF NOT EXISTS logs_mac ON logs(mac,id);", NULL, NULL, NULL) != SQLITE_OK) goto fail;
    /* IDs, unlike wall-clock seconds, distinguish rapid restarts and clock changes. */
    sqlite3_stmt *boundary = NULL;
    if (sqlite3_prepare_v2(log_db, "SELECT COALESCE(MAX(id),0) FROM logs;", -1, &boundary, NULL) != SQLITE_OK) goto fail;
    if (sqlite3_step(boundary) != SQLITE_ROW) { sqlite3_finalize(boundary); goto fail; }
    run_start_id = sqlite3_column_int64(boundary, 0);
    sqlite3_finalize(boundary);
    pthread_mutex_lock(&queue_mutex);
    enabled = 1; head = queued = 0; dropped = 0;
    pthread_mutex_unlock(&queue_mutex);
    last_cleanup = 0;
    return 0;
fail:
    fprintf(stderr, "[日志] 无法初始化日志数据库：%s\n", log_db ? sqlite3_errmsg(log_db) : "open failed");
    sqlite3_close(log_db); log_db = NULL;
    return -1;
}

void gateway_log_event(const char *level, const char *source, const char *event,
                       const char *mac, int short_addr, int64_t batch, const char *format, ...)
{
    GatewayLog row = {0};
    row.timestamp = (int64_t)time(NULL); row.short_addr = short_addr; row.batch = batch;
    snprintf(row.level, sizeof(row.level), "%s", level);
    snprintf(row.source, sizeof(row.source), "%s", source);
    snprintf(row.event, sizeof(row.event), "%s", event);
    snprintf(row.mac, sizeof(row.mac), "%s", mac ? mac : "");
    va_list args; va_start(args, format);
    vsnprintf(row.message, sizeof(row.message), format, args); va_end(args);
    pthread_mutex_lock(&queue_mutex);
    if (enabled) {
        if (queued == LOG_QUEUE_SIZE) ++dropped;
        else { queue[(head + queued) % LOG_QUEUE_SIZE] = row; ++queued; }
    }
    pthread_mutex_unlock(&queue_mutex);
}

uint64_t gateway_log_dropped(void)
{
    pthread_mutex_lock(&queue_mutex);
    uint64_t count = dropped;
    pthread_mutex_unlock(&queue_mutex);
    return count;
}

int gateway_log_flush(void)
{
    if (!log_db) return -1;
    GatewayLog rows[LOG_BATCH_SIZE];
    pthread_mutex_lock(&queue_mutex);
    unsigned count = queued < LOG_BATCH_SIZE ? queued : LOG_BATCH_SIZE;
    for (unsigned i = 0; i < count; ++i) rows[i] = queue[(head + i) % LOG_QUEUE_SIZE];
    pthread_mutex_unlock(&queue_mutex);
    sqlite3_stmt *stmt = NULL;
    if (count) {
        if (sqlite3_exec(log_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) return -1;
        if (sqlite3_prepare_v2(log_db, "INSERT INTO logs(timestamp,level,source,event,mac,short_addr,batch,message) VALUES(?,?,?,?,?,?,?,?);", -1, &stmt, NULL) != SQLITE_OK) goto rollback;
        for (unsigned i = 0; i < count; ++i) {
            GatewayLog *r = &rows[i];
            sqlite3_bind_int64(stmt, 1, r->timestamp);
            sqlite3_bind_text(stmt, 2, r->level, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, r->source, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, r->event, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, r->mac, -1, SQLITE_TRANSIENT);
            if (r->short_addr < 0) sqlite3_bind_null(stmt, 6); else sqlite3_bind_int(stmt, 6, r->short_addr);
            if (r->batch < 0) sqlite3_bind_null(stmt, 7); else sqlite3_bind_int64(stmt, 7, r->batch);
            sqlite3_bind_text(stmt, 8, r->message, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) != SQLITE_DONE) goto rollback;
            sqlite3_reset(stmt); sqlite3_clear_bindings(stmt);
        }
        sqlite3_finalize(stmt); stmt = NULL;
        if (sqlite3_exec(log_db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) goto rollback;
        pthread_mutex_lock(&queue_mutex);
        head = (head + count) % LOG_QUEUE_SIZE; queued -= count;
        pthread_mutex_unlock(&queue_mutex);
    }
    time_t now = time(NULL);
    if (now >= 1704067200 && (last_cleanup == 0 || now - last_cleanup >= 60)) {
        char sql[256];
        snprintf(sql, sizeof(sql), "DELETE FROM logs WHERE timestamp < %lld; DELETE FROM logs WHERE id <= COALESCE((SELECT id FROM logs ORDER BY id DESC LIMIT 1 OFFSET %d),-1);", (long long)now - LOG_KEEP_SECONDS, LOG_MAX_ROWS);
        if (sqlite3_exec(log_db, sql, NULL, NULL, NULL) != SQLITE_OK) return -1;
        last_cleanup = now;
    }
    return (int)count;
rollback:
    sqlite3_finalize(stmt);
    sqlite3_exec(log_db, "ROLLBACK;", NULL, NULL, NULL);
    return -1; /* Keep queued records for the next flush. */
}

static void bind_filter(sqlite3_stmt *stmt, const GatewayLogFilter *f)
{
    sqlite3_bind_text(stmt, 1, f->level ? f->level : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, f->source ? f->source : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, f->mac ? f->mac : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, f->search ? f->search : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, f->from); sqlite3_bind_int64(stmt, 6, f->to);
    sqlite3_bind_int64(stmt, 9, f->current_run_only ? run_start_id : 0);
}

int gateway_log_query(const GatewayLogFilter *f, GatewayLog *rows, int *total)
{
    if (!log_db || !f || !rows || !total || f->limit < 1 || f->limit > 1000 || f->offset < 0) return -1;
    const char *where = " WHERE id>?9 AND (?1='' OR level=?1) AND (?2='' OR source=?2) AND (?3='' OR mac=?3 COLLATE NOCASE) AND (?4='' OR instr(message,?4)>0 OR instr(event,?4)>0 OR instr(mac,?4)>0) AND (?5=0 OR timestamp>=?5) AND (?6=0 OR timestamp<=?6)";
    char sql[1024]; sqlite3_stmt *stmt = NULL;
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM logs%s;", where);
    if (sqlite3_prepare_v2(log_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    bind_filter(stmt, f);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return -1; }
    *total = sqlite3_column_int(stmt, 0); sqlite3_finalize(stmt);
    snprintf(sql, sizeof(sql), "SELECT id,timestamp,level,source,event,mac,short_addr,batch,message FROM logs%s ORDER BY id DESC LIMIT ?7 OFFSET ?8;", where);
    if (sqlite3_prepare_v2(log_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    bind_filter(stmt, f); sqlite3_bind_int(stmt, 7, f->limit); sqlite3_bind_int(stmt, 8, f->offset);
    int count = 0, rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && count < f->limit) {
        GatewayLog *r = &rows[count++]; memset(r, 0, sizeof(*r));
        r->id = sqlite3_column_int64(stmt, 0); r->timestamp = sqlite3_column_int64(stmt, 1);
        snprintf(r->level, sizeof(r->level), "%s", sqlite3_column_text(stmt, 2));
        snprintf(r->source, sizeof(r->source), "%s", sqlite3_column_text(stmt, 3));
        snprintf(r->event, sizeof(r->event), "%s", sqlite3_column_text(stmt, 4));
        snprintf(r->mac, sizeof(r->mac), "%s", sqlite3_column_text(stmt, 5));
        r->short_addr = sqlite3_column_type(stmt, 6) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt, 6);
        r->batch = sqlite3_column_type(stmt, 7) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 7);
        snprintf(r->message, sizeof(r->message), "%s", sqlite3_column_text(stmt, 8));
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? count : -1;
}

void gateway_log_close(void)
{
    pthread_mutex_lock(&queue_mutex); enabled = 0; pthread_mutex_unlock(&queue_mutex);
    while (gateway_log_flush() > 0) {}
    sqlite3_close(log_db); log_db = NULL;
}
