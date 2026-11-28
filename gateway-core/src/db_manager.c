#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sqlite3.h>
#include "db_manager.h"
#include "monitor_protocol.h"
#include "vibration_wire.h"

// 全局数据库句柄
sqlite3 *g_db = NULL;

int get_upload_timing(const char *mac,unsigned raw_required,UploadTimingRecord_t *out)
{
    if(!out)return -1;
    memset(out,0,sizeof(*out));
    if(!g_db || !mac || !*mac || raw_required>1U)return -1;
    sqlite3_stmt *s=NULL;
    const char *sql="SELECT mean_unit_ms,samples,last_duration_ms,last_blocks,last_batch_id "
                    "FROM upload_timing WHERE mac=? AND raw_required=?;";
    if(sqlite3_prepare_v2(g_db,sql,-1,&s,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,2,(int)raw_required);
    int rc=sqlite3_step(s),result=0;
    if(rc==SQLITE_ROW) {
        out->mean_unit_ms=(uint32_t)sqlite3_column_int64(s,0);
        out->samples=(uint32_t)sqlite3_column_int64(s,1);
        out->last_duration_ms=(uint32_t)sqlite3_column_int64(s,2);
        out->last_blocks=(uint32_t)sqlite3_column_int64(s,3);
        out->last_batch_id=(uint32_t)sqlite3_column_int64(s,4);result=1;
    } else if(rc!=SQLITE_DONE)result=-1;
    sqlite3_finalize(s);return result;
}

int save_upload_timing(const char *mac,uint32_t batch,unsigned raw_required,
                       unsigned blocks,uint64_t elapsed_ms,UploadTimingRecord_t *out)
{
    if(!g_db || !mac || !*mac || raw_required>1U || !blocks || blocks>65535U ||
       !elapsed_ms || elapsed_ms>UINT32_MAX || !out)return -1;
    uint64_t unit=raw_required?(elapsed_ms+blocks-1U)/blocks:elapsed_ms;
    /* Keep individual successful stalls from producing unbounded predictions.
     * Failed/expired transfers never reach this function. */
    if(unit<500U)unit=500U;
    if(unit>300000U)unit=300000U;
    sqlite3_mutex *mu=sqlite3_db_mutex(g_db);sqlite3_mutex_enter(mu);
    sqlite3_stmt *s=NULL;int result=-1;
    const char *sql="INSERT INTO upload_timing(mac,raw_required,mean_unit_ms,samples,"
        "last_duration_ms,last_blocks,last_batch_id,updated_at) VALUES(?,?,?,1,?,?,?,CAST(strftime('%s','now') AS INTEGER)) "
        "ON CONFLICT(mac,raw_required) DO UPDATE SET mean_unit_ms=(upload_timing.mean_unit_ms*3+excluded.mean_unit_ms+2)/4,"
        "samples=min(upload_timing.samples+1,2147483647),last_duration_ms=excluded.last_duration_ms,"
        "last_blocks=excluded.last_blocks,last_batch_id=excluded.last_batch_id,updated_at=excluded.updated_at "
        "WHERE upload_timing.last_batch_id<>excluded.last_batch_id;";
    if(sqlite3_prepare_v2(g_db,sql,-1,&s,NULL)!=SQLITE_OK)goto done;
    sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,2,(int)raw_required);
    sqlite3_bind_int64(s,3,(sqlite3_int64)unit);sqlite3_bind_int64(s,4,(sqlite3_int64)elapsed_ms);
    sqlite3_bind_int64(s,5,blocks);sqlite3_bind_int64(s,6,batch);
    if(sqlite3_step(s)!=SQLITE_DONE)goto done;
    result=sqlite3_changes(g_db)>0;
    sqlite3_finalize(s);s=NULL;
    if(get_upload_timing(mac,raw_required,out)!=1)result=-1;
done:
    sqlite3_finalize(s);sqlite3_mutex_leave(mu);return result;
}

int get_node_groups(NodeGroupRecord_t **records)
{
    sqlite3_stmt *s = NULL;
    *records = NULL;
    if (sqlite3_prepare_v2(g_db, "SELECT id,name,description FROM node_groups ORDER BY id", -1, &s, NULL) != SQLITE_OK) return -1;
    int count = 0, rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        void *next = realloc(*records, (size_t)(count + 1) * sizeof(**records));
        if (!next) { rc = SQLITE_NOMEM; break; }
        *records = next;
        NodeGroupRecord_t *r = &(*records)[count++];
        r->id = sqlite3_column_int(s, 0);
        snprintf(r->name, sizeof(r->name), "%s", sqlite3_column_text(s, 1));
        snprintf(r->description, sizeof(r->description), "%s", sqlite3_column_text(s, 2));
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) { free(*records); *records = NULL; return -1; }
    return count;
}

int get_node_group_members(int id, char (**macs)[32])
{
    sqlite3_stmt *s = NULL;
    *macs = NULL;
    if (sqlite3_prepare_v2(g_db, "SELECT mac FROM node_group_members WHERE group_id=? ORDER BY mac", -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(s, 1, id);
    int count = 0, rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        void *next = realloc(*macs, (size_t)(count + 1) * sizeof(**macs));
        if (!next) { rc = SQLITE_NOMEM; break; }
        *macs = next;
        snprintf((*macs)[count++], 32, "%s", sqlite3_column_text(s, 0));
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) { free(*macs); *macs = NULL; return -1; }
    return count;
}

int save_node_group(int *id, const char *name, const char *description,
                    const char *const *macs, int count)
{
    if (!id || *id < 0 || !name || !*name || strlen(name) > 128 ||
        !description || strlen(description) > 768 || count < 0 || (count && !macs)) return 2;
    sqlite3_mutex *mutex = sqlite3_db_mutex(g_db);
    sqlite3_mutex_enter(mutex);
    int result = -1, saved_id = *id;
    sqlite3_stmt *s = NULL;
    if (sqlite3_exec(g_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) goto done;
    /* Validate all members before replacing anything; never steal another group's node. */
    for (int i = 0; i < count; ++i) {
        if (!macs[i] || strlen(macs[i]) >= 32) { result = 2; goto rollback; }
        if (sqlite3_prepare_v2(g_db, "SELECT m.group_id FROM devices d LEFT JOIN node_group_members m ON m.mac=d.mac WHERE d.mac=?", -1, &s, NULL) != SQLITE_OK) goto rollback;
        sqlite3_bind_text(s, 1, macs[i], -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(s);
        if (rc == SQLITE_DONE) result = 2;
        else if (rc == SQLITE_ROW && sqlite3_column_type(s, 0) != SQLITE_NULL && sqlite3_column_int(s, 0) != saved_id) result = 1;
        else if (rc != SQLITE_ROW) result = -1;
        else result = 0;
        sqlite3_finalize(s); s = NULL;
        if (result) goto rollback;
    }
    result = -1;
    MonitorPolicyRecord_t inherited;
    int has_sampling = get_node_group_sampling(saved_id, &inherited);
    if (has_sampling < 0) goto rollback;
    if (has_sampling) {
        for (int i = 0; i < count; ++i) {
            if (sqlite3_prepare_v2(g_db, "SELECT 1 FROM node_group_members WHERE mac=? AND group_id=?", -1, &s, NULL) != SQLITE_OK) goto rollback;
            sqlite3_bind_text(s, 1, macs[i], -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(s, 2, saved_id);
            int rc = sqlite3_step(s);
            sqlite3_finalize(s); s = NULL;
            if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto rollback;
            if (rc == SQLITE_DONE) {
                MonitorPolicyRecord_t copy = inherited;
                if (save_device_monitor_policy(macs[i], &copy, NULL) != 0) goto rollback;
            }
        }
    }
    if (sqlite3_prepare_v2(g_db, saved_id ? "UPDATE node_groups SET name=?,description=? WHERE id=?" : "INSERT INTO node_groups(name,description) VALUES(?,?)", -1, &s, NULL) != SQLITE_OK) goto rollback;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, description, -1, SQLITE_TRANSIENT);
    if (saved_id) sqlite3_bind_int(s, 3, saved_id);
    if (sqlite3_step(s) != SQLITE_DONE) goto rollback;
    if (saved_id && sqlite3_changes(g_db) != 1) { result = 2; goto rollback; }
    sqlite3_finalize(s); s = NULL;
    if (!saved_id) saved_id = (int)sqlite3_last_insert_rowid(g_db);
    if (sqlite3_prepare_v2(g_db, "DELETE FROM node_group_members WHERE group_id=?", -1, &s, NULL) != SQLITE_OK) goto rollback;
    sqlite3_bind_int(s, 1, saved_id);
    if (sqlite3_step(s) != SQLITE_DONE) goto rollback;
    sqlite3_finalize(s); s = NULL;
    if (sqlite3_prepare_v2(g_db, "INSERT INTO node_group_members(mac,group_id) VALUES(?,?)", -1, &s, NULL) != SQLITE_OK) goto rollback;
    for (int i = 0; i < count; ++i) {
        sqlite3_bind_text(s, 1, macs[i], -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(s, 2, saved_id);
        if (sqlite3_step(s) != SQLITE_DONE) { result = 2; goto rollback; }
        sqlite3_reset(s); sqlite3_clear_bindings(s);
    }
    sqlite3_finalize(s); s = NULL;
    if (sqlite3_exec(g_db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto rollback;
    *id = saved_id; result = 0; goto done;
rollback:
    sqlite3_finalize(s); s = NULL;
    sqlite3_exec(g_db, "ROLLBACK", NULL, NULL, NULL);
done:
    sqlite3_mutex_leave(mutex);
    return result;
}

int delete_node_group(int id)
{
    sqlite3_stmt *s = NULL;
    if (id <= 0 || sqlite3_prepare_v2(g_db, "DELETE FROM node_groups WHERE id=?", -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(s, 1, id);
    int rc = sqlite3_step(s), changed = sqlite3_changes(g_db);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE && changed == 1 ? 0 : -1;
}

#ifndef GATEWAY_DATABASE_PATH
#define GATEWAY_DATABASE_PATH "data/gateway.db"
#endif

#define MIN_VALID_HISTORY_EPOCH 1704067200LL //2024-01-01 00:00:00 UTC

// 无RTC设备在浏览器授时前仍更新当前值，但不写入带1970时间戳的历史记录。
static int history_time_is_valid(void)
{
    return (long long)time(NULL) >= MIN_VALID_HISTORY_EPOCH;
}


void build_default_device_name(const char *model_name,
                               const char *mac,
                               char *out_name,
                               size_t out_name_size)
{
    if (!out_name || out_name_size == 0)
    {
        return;
    }

    const char *safe_model =
        (model_name && model_name[0] != '\0') ? model_name : "Unknown Device";
    char short_id[5] = {0};
    size_t short_id_len = 0;

    if (mac)
    {
        for (const char *p = mac; *p && short_id_len < 4; p++)
        {
            if (isxdigit((unsigned char)*p))
            {
                short_id[short_id_len++] =
                    (char)toupper((unsigned char)*p);
            }
        }
    }

    if (short_id_len == 0)
    {
        snprintf(out_name, out_name_size, "%s", safe_model);
    }
    else
    {
        snprintf(out_name, out_name_size, "%s-%s", safe_model, short_id);
    }
}


// 检查某张表中是否已经存在指定字段
static int column_exists(const char *table_name, const char *column_name)
{
    char sql[128];
    sqlite3_stmt *stmt = NULL;
    int found = 0;

    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table_name);

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        return 0;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *name = (const char *)sqlite3_column_text(stmt, 1);

        if (name && strcmp(name, column_name) == 0)
        {
            found = 1;
            break;
        }
    }

    sqlite3_finalize(stmt);
    return found;
}


// 初始化并创建数据库和表
int init_database(void) 
{
    int rc;
    int migrate_join_confirmation = 0;
    char *err_msg = 0;

    rc = sqlite3_open(GATEWAY_DATABASE_PATH, &g_db);
    if (rc != SQLITE_OK) 
    {
        fprintf(stderr, "❌ [数据库] 无法打开数据库: %s\n", sqlite3_errmsg(g_db));
        sqlite3_close(g_db);
        return -1;
    }

    // 开启 SQLite 外键约束，使删除设备时可以自动级联删除对应的历史数据
    rc = sqlite3_exec(g_db, "PRAGMA foreign_keys = ON;", 0, 0, &err_msg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 开启外键约束失败: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    // 采用单表设备影子架构，直接追加 temp、mag 和 work_mode 字段
    const char *sql_create_devices =
        "CREATE TABLE IF NOT EXISTS devices ("
        "name TEXT, "
        "custom_name TEXT DEFAULT NULL, "
        "mac TEXT PRIMARY KEY, "
        "short_addr TEXT, "
        "status TEXT DEFAULT 'offline', "
        "temp TEXT DEFAULT '--', "
        "mag TEXT DEFAULT '--', "
        "digital_db TEXT DEFAULT '--', "
        "analog_db TEXT DEFAULT '--', "
        "work_mode TEXT DEFAULT 'sleep', "
        "monitoring_enabled INTEGER NOT NULL DEFAULT 0, "
        "collection_state TEXT NOT NULL DEFAULT 'idle', "
        "last_collection INTEGER DEFAULT 0, "
        "last_active INTEGER DEFAULT 0, " // 记录最后一次心跳的 Unix 时间戳
        "expected_interval_sec INTEGER NOT NULL DEFAULT 300, "
        "monitor_policy_id INTEGER NOT NULL DEFAULT 1, "
        "raw_upload_generation INTEGER NOT NULL DEFAULT 1"
        ");";

    rc = sqlite3_exec(g_db, sql_create_devices, 0, 0, &err_msg);
    if (rc != SQLITE_OK) 
    {
        fprintf(stderr, "❌ [数据库] 创建 devices 表失败: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    if (sqlite3_exec(g_db,
        "CREATE TABLE IF NOT EXISTS node_interview (mac TEXT PRIMARY KEY REFERENCES devices(mac) ON DELETE CASCADE,json TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS node_groups (id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL,description TEXT NOT NULL DEFAULT '');"
        "CREATE TABLE IF NOT EXISTS node_group_members (mac TEXT PRIMARY KEY "
        "REFERENCES devices(mac) ON DELETE CASCADE,group_id INTEGER NOT NULL "
        "REFERENCES node_groups(id) ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS node_group_members_group ON node_group_members(group_id);"
        "CREATE TABLE IF NOT EXISTS upload_timing (mac TEXT NOT NULL REFERENCES devices(mac) ON DELETE CASCADE,"
        "raw_required INTEGER NOT NULL CHECK(raw_required IN (0,1)),mean_unit_ms INTEGER NOT NULL CHECK(mean_unit_ms>0),"
        "samples INTEGER NOT NULL,last_duration_ms INTEGER NOT NULL,last_blocks INTEGER NOT NULL,"
        "last_batch_id INTEGER NOT NULL,updated_at INTEGER NOT NULL,PRIMARY KEY(mac,raw_required));"
        "CREATE TABLE IF NOT EXISTS node_group_sampling (group_id INTEGER PRIMARY KEY "
        "REFERENCES node_groups(id) ON DELETE CASCADE,auto_interval_sec INTEGER NOT NULL,"
        "vibration_window_ms INTEGER NOT NULL,digital_mic_window_ms INTEGER NOT NULL,"
        "analog_mic_window_ms INTEGER NOT NULL,raw_upload_every_cycles INTEGER NOT NULL,"
        "sensor_enable_mask INTEGER NOT NULL);",
        NULL, NULL, NULL) != SQLITE_OK) return -1;

    /* Announcements remain routable internally, but are not visible devices.
     * Preserve the proof independently from later sleep/offline/interview state. */
    migrate_join_confirmation = !column_exists("devices", "join_confirmed");
    /* Add telemetry without replacing the existing database or device records. */
    const char *telemetry_names[] = {"lqi", "rssi", "power_source", "link_updated_at", "description", "collection_batch_id", "capture_batch_id", "capture_pending"};
    const char *telemetry_sql[] = {
        "ALTER TABLE devices ADD COLUMN lqi INTEGER NOT NULL DEFAULT -1;",
        "ALTER TABLE devices ADD COLUMN rssi INTEGER NOT NULL DEFAULT -128;",
        "ALTER TABLE devices ADD COLUMN power_source INTEGER NOT NULL DEFAULT 0;",
        "ALTER TABLE devices ADD COLUMN link_updated_at INTEGER NOT NULL DEFAULT 0;",
        "ALTER TABLE devices ADD COLUMN description TEXT NOT NULL DEFAULT '';",
        "ALTER TABLE devices ADD COLUMN collection_batch_id INTEGER NOT NULL DEFAULT 0;",
        "ALTER TABLE devices ADD COLUMN capture_batch_id INTEGER NOT NULL DEFAULT 0;",
        "ALTER TABLE devices ADD COLUMN capture_pending INTEGER NOT NULL DEFAULT 0;"
    };
    for (size_t i = 0; i < sizeof(telemetry_names) / sizeof(telemetry_names[0]); ++i) {
        if (!column_exists("devices", telemetry_names[i])) {
            rc = sqlite3_exec(g_db, telemetry_sql[i], 0, 0, &err_msg);
            if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
        }
    }

    // 兼容旧版 gateway.db：如果原来的 devices 表没有 work_mode 字段，则自动补上
    if (!column_exists("devices", "work_mode"))
    {
        const char *sql_add_work_mode =
            "ALTER TABLE devices ADD COLUMN work_mode TEXT DEFAULT 'sleep';";

        rc = sqlite3_exec(g_db, sql_add_work_mode, 0, 0, &err_msg);
        if (rc != SQLITE_OK)
        {
            fprintf(stderr, "❌ [数据库] 增加 work_mode 字段失败: %s\n", err_msg);
            sqlite3_free(err_msg);
            return -1;
        }

        printf("✅ [数据库] 已为旧 devices 表增加 work_mode 字段。\n");
    }

    // 兼容旧版 gateway.db：name 继续保存产品型号，自定义名称单独保存
    if (!column_exists("devices", "custom_name"))
    {
        const char *sql_add_custom_name =
            "ALTER TABLE devices ADD COLUMN custom_name TEXT DEFAULT NULL;";

        rc = sqlite3_exec(g_db, sql_add_custom_name, 0, 0, &err_msg);
        if (rc != SQLITE_OK)
        {
            fprintf(stderr, "❌ [数据库] 增加 custom_name 字段失败: %s\n", err_msg);
            sqlite3_free(err_msg);
            return -1;
        }

        printf("✅ [数据库] 已为旧 devices 表增加 custom_name 字段。\n");
    }

    // 兼容旧数据库：麦克风先保存最新的一秒声级，不改变已有温度和磁场历史表。
    if (!column_exists("devices", "digital_db"))
    {
        rc = sqlite3_exec(g_db,
                          "ALTER TABLE devices ADD COLUMN digital_db TEXT DEFAULT '--';",
                          0, 0, &err_msg);
        if (rc != SQLITE_OK)
        {
            fprintf(stderr, "❌ [数据库] 增加 digital_db 字段失败: %s\n", err_msg);
            sqlite3_free(err_msg);
            return -1;
        }
    }

    if (!column_exists("devices", "analog_db"))
    {
        rc = sqlite3_exec(g_db,
                          "ALTER TABLE devices ADD COLUMN analog_db TEXT DEFAULT '--';",
                          0, 0, &err_msg);
        if (rc != SQLITE_OK)
        {
            fprintf(stderr, "❌ [数据库] 增加 analog_db 字段失败: %s\n", err_msg);
            sqlite3_free(err_msg);
            return -1;
        }
    }

    if (!column_exists("devices", "monitoring_enabled"))
    {
        rc = sqlite3_exec(g_db,
                          "ALTER TABLE devices ADD COLUMN monitoring_enabled INTEGER NOT NULL DEFAULT 0;",
                          0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }
    if (!column_exists("devices", "collection_state"))
    {
        rc = sqlite3_exec(g_db,
                          "ALTER TABLE devices ADD COLUMN collection_state TEXT NOT NULL DEFAULT 'idle';",
                          0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }
    if (!column_exists("devices", "last_collection"))
    {
        rc = sqlite3_exec(g_db,
                          "ALTER TABLE devices ADD COLUMN last_collection INTEGER DEFAULT 0;",
                          0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }

    const char *sql_create_custom_name_index =
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_devices_custom_name_unique "
        "ON devices(custom_name COLLATE NOCASE) "
        "WHERE custom_name IS NOT NULL AND custom_name <> '';";

    rc = sqlite3_exec(g_db, sql_create_custom_name_index, 0, 0, &err_msg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 创建自定义名称唯一索引失败: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    /* 撤销记录不引用 devices。业务设备被删除后，该保护记录仍要长期存在，
     * 用来拦截没有收到第一次离网命令的休眠节点。 */
    const char *sql_create_revoked_nodes =
        "CREATE TABLE IF NOT EXISTS revoked_nodes ("
        "mac TEXT PRIMARY KEY COLLATE NOCASE,"
        "last_short_addr INTEGER DEFAULT NULL,"
        "state TEXT NOT NULL DEFAULT 'pending' "
            "CHECK(state IN ('pending','sending','guard')),"
        "deleted_at INTEGER NOT NULL,"
        "last_seen_at INTEGER NOT NULL,"
        "last_leave_at INTEGER NOT NULL DEFAULT 0,"
        "retry_count INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS idx_revoked_nodes_short "
        "ON revoked_nodes(last_short_addr) WHERE last_short_addr IS NOT NULL;";
    rc = sqlite3_exec(g_db, sql_create_revoked_nodes, 0, 0, &err_msg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 创建节点撤销保护表失败: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    // 创建温度历史数据表
    const char *sql_create_temperature =
        "CREATE TABLE IF NOT EXISTS temperature_data ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "mac TEXT NOT NULL, "
        "temperature REAL NOT NULL, "
        "timestamp INTEGER NOT NULL, "
        "FOREIGN KEY(mac) REFERENCES devices(mac) ON DELETE CASCADE"
        ");";

    rc = sqlite3_exec(g_db, sql_create_temperature, 0, 0, &err_msg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 创建 temperature_data 表失败: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    // 创建磁场历史数据表
    const char *sql_create_magnetic =
        "CREATE TABLE IF NOT EXISTS magnetic_data ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "mac TEXT NOT NULL, "
        "mag_x REAL NOT NULL, "
        "mag_y REAL NOT NULL, "
        "mag_z REAL NOT NULL, "
        "timestamp INTEGER NOT NULL, "
        "FOREIGN KEY(mac) REFERENCES devices(mac) ON DELETE CASCADE"
        ");";

    rc = sqlite3_exec(g_db, sql_create_magnetic, 0, 0, &err_msg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 创建 magnetic_data 表失败: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    // 创建麦克风历史表；source为1表示数字麦克风，0表示超声麦克风
    const char *sql_create_sound =
        "CREATE TABLE IF NOT EXISTS sound_data ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "mac TEXT NOT NULL, "
        "source INTEGER NOT NULL CHECK(source IN (0, 1)), "
        "db_spl REAL NOT NULL, "
        "timestamp INTEGER NOT NULL, "
        "FOREIGN KEY(mac) REFERENCES devices(mac) ON DELETE CASCADE"
        ");";

    rc = sqlite3_exec(g_db, sql_create_sound, 0, 0, &err_msg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 创建 sound_data 表失败: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    const char *sql_create_monitor_config =
        "CREATE TABLE IF NOT EXISTS monitor_config ("
        "id INTEGER PRIMARY KEY CHECK(id = 1), "
        "auto_interval_sec INTEGER NOT NULL, poll_interval_sec INTEGER NOT NULL, "
        "vibration_rate_hz INTEGER NOT NULL, digital_mic_rate_hz INTEGER NOT NULL, "
        "analog_mic_rate_hz INTEGER NOT NULL, mic_window_ms INTEGER NOT NULL, "
        "vibration_window_ms INTEGER NOT NULL DEFAULT 500, "
        "digital_mic_window_ms INTEGER NOT NULL DEFAULT 1000, "
        "analog_mic_window_ms INTEGER NOT NULL DEFAULT 1000, "
        "post_upload_rx_window_sec INTEGER NOT NULL DEFAULT 5);";
    rc = sqlite3_exec(g_db, sql_create_monitor_config, 0, 0, &err_msg);
    if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    int had_vibration_window = column_exists("monitor_config", "vibration_window_ms");
    int had_digital_window = column_exists("monitor_config", "digital_mic_window_ms");
    int had_analog_window = column_exists("monitor_config", "analog_mic_window_ms");
    int had_post_upload_rx_window =
        column_exists("monitor_config", "post_upload_rx_window_sec");
    if (!had_vibration_window)
    {
        rc = sqlite3_exec(g_db, "ALTER TABLE monitor_config ADD COLUMN vibration_window_ms INTEGER NOT NULL DEFAULT 500;", 0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }
    if (!column_exists("devices", "expected_interval_sec"))
    {
        rc = sqlite3_exec(g_db,
                          "ALTER TABLE devices ADD COLUMN expected_interval_sec INTEGER NOT NULL DEFAULT 300;",
                          0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
        /* 升级旧数据库时使用当前策略作为这些节点最后已知周期；节点下次
         * 0x99查询成功后会分别刷新，避免长周期节点被按30分钟默认值误判。 */
        rc = sqlite3_exec(g_db,
                          "UPDATE devices SET expected_interval_sec="
                          "COALESCE((SELECT auto_interval_sec FROM monitor_config WHERE id=1),300);",
                          0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }
    if (!column_exists("devices", "monitor_policy_id"))
    {
        rc = sqlite3_exec(g_db,
                          "ALTER TABLE devices ADD COLUMN monitor_policy_id INTEGER NOT NULL DEFAULT 1;",
                          0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }
    if (!column_exists("devices", "raw_upload_generation"))
    {
        rc = sqlite3_exec(g_db,
                          "ALTER TABLE devices ADD COLUMN raw_upload_generation INTEGER NOT NULL DEFAULT 1;",
                          0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }

    /* Keep the user's desired switch across backend restarts. */
    if (!had_digital_window)
    {
        rc = sqlite3_exec(g_db, "ALTER TABLE monitor_config ADD COLUMN digital_mic_window_ms INTEGER NOT NULL DEFAULT 1000;", 0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }
    if (!had_analog_window)
    {
        rc = sqlite3_exec(g_db, "ALTER TABLE monitor_config ADD COLUMN analog_mic_window_ms INTEGER NOT NULL DEFAULT 1000;", 0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }
    if (!had_post_upload_rx_window)
    {
        rc = sqlite3_exec(g_db, "ALTER TABLE monitor_config ADD COLUMN post_upload_rx_window_sec INTEGER NOT NULL DEFAULT 5;", 0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }
    rc = sqlite3_exec(g_db,
        "INSERT OR IGNORE INTO monitor_config "
        "(id,auto_interval_sec,poll_interval_sec,vibration_rate_hz,digital_mic_rate_hz,"
        "analog_mic_rate_hz,mic_window_ms,vibration_window_ms,digital_mic_window_ms,"
        "analog_mic_window_ms,post_upload_rx_window_sec) "
        "VALUES(1,300,5,26667,46875,256000,1000,500,1000,1000,5);",
        0, 0, &err_msg);
    if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    /* 老版本只有mic_window_ms；升级时把已有窗口值复制给三个传感器，
     * 避免数据库升级把用户原来的0.5/1.5秒设置重置为1秒。 */
    if (!had_vibration_window && sqlite3_changes(g_db) == 0)
    {
        rc = sqlite3_exec(g_db, "UPDATE monitor_config SET vibration_window_ms=mic_window_ms WHERE id=1;", 0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }
    if (!had_digital_window)
    {
        rc = sqlite3_exec(g_db, "UPDATE monitor_config SET digital_mic_window_ms=mic_window_ms WHERE id=1;", 0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }
    if (!had_analog_window)
    {
        rc = sqlite3_exec(g_db, "UPDATE monitor_config SET analog_mic_window_ms=mic_window_ms WHERE id=1;", 0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }
    /* 采样率由硬件链路固定；迁移旧数据库时统一纠正历史配置。 */
    rc = sqlite3_exec(g_db,
        "UPDATE monitor_config SET vibration_rate_hz=26667,digital_mic_rate_hz=46875,analog_mic_rate_hz=256000,mic_window_ms=digital_mic_window_ms WHERE id=1;",
        0, 0, &err_msg);
    if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }

    const char *sql_create_monitor_policies =
        "CREATE TABLE IF NOT EXISTS monitor_policies ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL,"
        "is_default INTEGER NOT NULL DEFAULT 0 CHECK(is_default IN (0,1)),"
        "auto_interval_sec INTEGER NOT NULL,"
        "poll_interval_sec INTEGER NOT NULL DEFAULT 5,"
        "vibration_rate_hz INTEGER NOT NULL DEFAULT 26667,"
        "digital_mic_rate_hz INTEGER NOT NULL DEFAULT 46875,"
        "analog_mic_rate_hz INTEGER NOT NULL DEFAULT 256000,"
        "vibration_window_ms INTEGER NOT NULL DEFAULT 500,"
        "digital_mic_window_ms INTEGER NOT NULL DEFAULT 1000,"
        "analog_mic_window_ms INTEGER NOT NULL DEFAULT 1000,"
        "raw_upload_every_cycles INTEGER NOT NULL DEFAULT 1 CHECK(raw_upload_every_cycles BETWEEN 1 AND 65535),"
        "post_upload_rx_window_sec INTEGER NOT NULL DEFAULT 5,"
        "sensor_enable_mask INTEGER NOT NULL DEFAULT 7 CHECK(sensor_enable_mask BETWEEN 0 AND 7)"
        ");";
    rc = sqlite3_exec(g_db, sql_create_monitor_policies, 0, 0, &err_msg);
    if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    if (!column_exists("monitor_policies", "post_upload_rx_window_sec"))
    {
        rc = sqlite3_exec(g_db, "ALTER TABLE monitor_policies ADD COLUMN post_upload_rx_window_sec INTEGER NOT NULL DEFAULT 5;", 0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }
    if (!column_exists("monitor_policies", "sensor_enable_mask"))
    {
        rc = sqlite3_exec(g_db, "ALTER TABLE monitor_policies ADD COLUMN sensor_enable_mask INTEGER NOT NULL DEFAULT 7;", 0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }
    rc = sqlite3_exec(g_db,
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_monitor_policy_default "
        "ON monitor_policies(is_default) WHERE is_default=1;",
        0, 0, &err_msg);
    if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    /* Dedicated policies belong to exactly one real device; remove with it. */
    if (!column_exists("monitor_policies", "owner_mac")) {
        rc = sqlite3_exec(g_db,
            "ALTER TABLE monitor_policies ADD COLUMN owner_mac TEXT REFERENCES devices(mac) ON DELETE CASCADE;",
            0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }
    rc = sqlite3_exec(g_db,
        "CREATE UNIQUE INDEX IF NOT EXISTS monitor_policy_owner ON monitor_policies(owner_mac) WHERE owner_mac IS NOT NULL;",
        0, 0, &err_msg);
    if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    rc = sqlite3_exec(g_db,
        "INSERT OR IGNORE INTO monitor_policies "
        "(id,name,is_default,auto_interval_sec,poll_interval_sec,vibration_rate_hz,"
        "digital_mic_rate_hz,analog_mic_rate_hz,vibration_window_ms,"
        "digital_mic_window_ms,analog_mic_window_ms,raw_upload_every_cycles,"
        "post_upload_rx_window_sec,sensor_enable_mask) "
        "SELECT 1,'默认采样配置',1,auto_interval_sec,poll_interval_sec,"
        "vibration_rate_hz,digital_mic_rate_hz,analog_mic_rate_hz,"
        "vibration_window_ms,digital_mic_window_ms,analog_mic_window_ms,1,"
        "post_upload_rx_window_sec,7 "
        "FROM monitor_config WHERE id=1;",
        0, 0, &err_msg);
    if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    /* Repair intervals shorter than the supported minimum while preserving
     * saved one-minute policies and the five-minute default. */
    rc = sqlite3_exec(g_db,
        "UPDATE monitor_policies SET name='默认采样配置' "
        "WHERE is_default=1 AND name='默认监测策略';"
        "UPDATE monitor_config SET auto_interval_sec=300 WHERE auto_interval_sec<60;"
        "UPDATE monitor_policies SET auto_interval_sec=300 WHERE auto_interval_sec<60;"
        "UPDATE devices SET expected_interval_sec=300 WHERE expected_interval_sec<60;",
        0, 0, &err_msg);
    if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    rc = sqlite3_exec(g_db,
        "UPDATE devices SET monitor_policy_id=1 WHERE monitor_policy_id NOT IN "
        "(SELECT id FROM monitor_policies);",
        0, 0, &err_msg);
    if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }

    const char *sql_create_zigbee_config =
        "CREATE TABLE IF NOT EXISTS zigbee_config ("
        "id INTEGER PRIMARY KEY CHECK(id=1),"
        "channel_strategy TEXT NOT NULL DEFAULT 'auto' CHECK(channel_strategy IN ('auto','manual')) ,"
        "target_channel INTEGER DEFAULT NULL CHECK(target_channel IS NULL OR target_channel BETWEEN 11 AND 26),"
        "actual_pan_id INTEGER DEFAULT NULL,"
        "actual_channel INTEGER DEFAULT NULL CHECK(actual_channel IS NULL OR actual_channel BETWEEN 11 AND 26),"
        "startup_source TEXT NOT NULL DEFAULT 'unknown');"
        "INSERT OR IGNORE INTO zigbee_config(id,channel_strategy,target_channel) VALUES(1,'auto',NULL);";
    rc = sqlite3_exec(g_db, sql_create_zigbee_config, 0, 0, &err_msg);
    if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    if (!column_exists("zigbee_config", "actual_pan_id"))
    {
        rc = sqlite3_exec(g_db, "ALTER TABLE zigbee_config ADD COLUMN actual_pan_id INTEGER DEFAULT NULL;", 0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }
    if (!column_exists("zigbee_config", "actual_channel"))
    {
        rc = sqlite3_exec(g_db, "ALTER TABLE zigbee_config ADD COLUMN actual_channel INTEGER DEFAULT NULL;", 0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }
    if (!column_exists("zigbee_config", "startup_source"))
    {
        rc = sqlite3_exec(g_db, "ALTER TABLE zigbee_config ADD COLUMN startup_source TEXT NOT NULL DEFAULT 'unknown';", 0, 0, &err_msg);
        if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    }

    /* One active/recent channel operation is retained so a long migration can
     * resume after the Linux service restarts. Nodes may already be waiting on
     * the target channel, therefore losing this progress would split the net. */
    const char *sql_create_channel_operation =
        "CREATE TABLE IF NOT EXISTS channel_operation ("
        "id INTEGER PRIMARY KEY CHECK(id=1),"
        "state TEXT NOT NULL DEFAULT 'idle',strategy INTEGER NOT NULL DEFAULT 0,"
        "target_channel INTEGER NOT NULL DEFAULT 0,current_channel INTEGER NOT NULL DEFAULT 0,"
        "coordinator_status INTEGER NOT NULL DEFAULT 255,"
        "waiting_for_nodes INTEGER NOT NULL DEFAULT 0,commit_sent INTEGER NOT NULL DEFAULT 0,"
        "node_count INTEGER NOT NULL DEFAULT 0,prepared_count INTEGER NOT NULL DEFAULT 0,"
        "message TEXT NOT NULL DEFAULT '');"
        "INSERT OR IGNORE INTO channel_operation(id) VALUES(1);"
        "CREATE TABLE IF NOT EXISTS channel_operation_nodes ("
        "mac TEXT PRIMARY KEY,short_addr INTEGER NOT NULL,state INTEGER NOT NULL DEFAULT 0);";
    rc = sqlite3_exec(g_db, sql_create_channel_operation, 0, 0, &err_msg);
    if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }

    const char *sql_create_vibration =
        "CREATE TABLE IF NOT EXISTS vibration_batches ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, mac TEXT NOT NULL, batch_id INTEGER NOT NULL, "
        "sample_rate_hz INTEGER NOT NULL, sample_count INTEGER NOT NULL, samples BLOB NOT NULL, "
        "features BLOB NOT NULL, timestamp INTEGER NOT NULL, "
        "FOREIGN KEY(mac) REFERENCES devices(mac) ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS idx_vibration_mac_time ON vibration_batches(mac,timestamp);";
    rc = sqlite3_exec(g_db, sql_create_vibration, 0, 0, &err_msg);
    if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }

    rc = sqlite3_exec(g_db,
        "CREATE TABLE IF NOT EXISTS upload_batch_receipts ("
        "mac TEXT NOT NULL,batch_id INTEGER NOT NULL,identity BLOB NOT NULL,"
        "raw_required INTEGER NOT NULL,manifest BLOB,PRIMARY KEY(mac,batch_id),"
        "FOREIGN KEY(mac) REFERENCES devices(mac) ON DELETE CASCADE);"
        "CREATE TRIGGER IF NOT EXISTS upload_receipt_vibration_delete AFTER DELETE ON vibration_batches BEGIN "
        "DELETE FROM upload_batch_receipts WHERE mac=OLD.mac AND batch_id=OLD.batch_id;END;",
        0, 0, &err_msg);
    if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }

    const char *sql_create_collection_values =
        "CREATE TABLE IF NOT EXISTS gateway_exchange_sequence ("
        "mac TEXT NOT NULL REFERENCES devices(mac) ON DELETE CASCADE,"
        "token INTEGER NOT NULL, seq INTEGER NOT NULL, touched INTEGER NOT NULL,"
        "PRIMARY KEY(mac,token));"
        "CREATE TABLE IF NOT EXISTS gateway_exchange_command ("
        "mac TEXT NOT NULL,token INTEGER NOT NULL,frame BLOB NOT NULL,PRIMARY KEY(mac,token),"
        "FOREIGN KEY(mac,token) REFERENCES gateway_exchange_sequence(mac,token) ON DELETE CASCADE);"
        "CREATE TABLE IF NOT EXISTS sampling_resume_anchor ("
        "mac TEXT PRIMARY KEY,anchor_ms INTEGER NOT NULL,"
        "FOREIGN KEY(mac) REFERENCES devices(mac) ON DELETE CASCADE);"
        "CREATE TABLE IF NOT EXISTS collection_capture_times ("
        "mac TEXT NOT NULL,batch_id INTEGER NOT NULL,captured_at INTEGER NOT NULL,"
        "PRIMARY KEY(mac,batch_id),"
        "FOREIGN KEY(mac) REFERENCES devices(mac) ON DELETE CASCADE);"
        "CREATE TABLE IF NOT EXISTS collection_values ("
        "mac TEXT NOT NULL,batch_id INTEGER NOT NULL,temperature REAL,"
        "mag_x REAL,mag_y REAL,mag_z REAL,digital_db REAL,analog_db REAL,"
        "timestamp INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER)),"
        "PRIMARY KEY(mac,batch_id),"
        "FOREIGN KEY(mac) REFERENCES devices(mac) ON DELETE CASCADE);";
    rc = sqlite3_exec(g_db, sql_create_collection_values, 0, 0, &err_msg);
    if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }
    rc = sqlite3_exec(g_db,
        "CREATE INDEX IF NOT EXISTS idx_collection_values_mac_time ON collection_values(mac,timestamp);",
        0, 0, &err_msg);
    if (rc != SQLITE_OK) { sqlite3_free(err_msg); return -1; }

    // 为温度历史建立 MAC + 时间戳联合索引
    const char *sql_create_temperature_index =
        "CREATE INDEX IF NOT EXISTS idx_temperature_mac_time "
        "ON temperature_data(mac, timestamp);";

    rc = sqlite3_exec(g_db, sql_create_temperature_index, 0, 0, &err_msg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 创建温度历史索引失败: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    // 为磁场历史建立 MAC + 时间戳联合索引
    const char *sql_create_magnetic_index =
        "CREATE INDEX IF NOT EXISTS idx_magnetic_mac_time "
        "ON magnetic_data(mac, timestamp);";

    rc = sqlite3_exec(g_db, sql_create_magnetic_index, 0, 0, &err_msg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 创建磁场历史索引失败: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    // 麦克风历史按设备和时间查询，并在同一时刻区分两路来源
    const char *sql_create_sound_index =
        "CREATE INDEX IF NOT EXISTS idx_sound_mac_time_source "
        "ON sound_data(mac, timestamp, source);";

    rc = sqlite3_exec(g_db, sql_create_sound_index, 0, 0, &err_msg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 创建麦克风历史索引失败: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    if (migrate_join_confirmation) {
        /* Do not trust a name, a short address, online status or cached LQI.
         * Older ready interviews are serialized by cJSON without whitespace.
         * Historical acquisition data also proves a completed application join. */
        const char *migration =
            "BEGIN IMMEDIATE;"
            "ALTER TABLE devices ADD COLUMN join_confirmed INTEGER NOT NULL DEFAULT 0;"
            "UPDATE devices SET join_confirmed=1 WHERE last_collection>0 "
            "OR EXISTS(SELECT 1 FROM node_interview i WHERE i.mac=devices.mac AND i.json LIKE '%\"state\":\"ready\"%') "
            "OR EXISTS(SELECT 1 FROM temperature_data t WHERE t.mac=devices.mac) "
            "OR EXISTS(SELECT 1 FROM magnetic_data m WHERE m.mac=devices.mac) "
            "OR EXISTS(SELECT 1 FROM sound_data s WHERE s.mac=devices.mac) "
            "OR EXISTS(SELECT 1 FROM collection_values c WHERE c.mac=devices.mac) "
            "OR EXISTS(SELECT 1 FROM vibration_batches v WHERE v.mac=devices.mac);"
            "COMMIT;";
        if (sqlite3_exec(g_db, migration, NULL, NULL, NULL) != SQLITE_OK) {
            (void)sqlite3_exec(g_db,"ROLLBACK;",NULL,NULL,NULL);return -1;
        }
    }
    if (!column_exists("devices", "user_authorized")) {
        const char *migration =
            "BEGIN IMMEDIATE;"
            "ALTER TABLE devices ADD COLUMN user_authorized INTEGER NOT NULL DEFAULT 0;"
            "UPDATE devices SET user_authorized=1,monitoring_enabled=1 WHERE join_confirmed=1;"
            "COMMIT;";
        if(sqlite3_exec(g_db,migration,NULL,NULL,NULL)!=SQLITE_OK) {
            (void)sqlite3_exec(g_db,"ROLLBACK;",NULL,NULL,NULL);return -1;
        }
    }
    if (!column_exists("devices", "sampling_admission") && sqlite3_exec(g_db,
        "ALTER TABLE devices ADD COLUMN sampling_admission INTEGER NOT NULL DEFAULT 0;",
        NULL,NULL,NULL)!=SQLITE_OK) return -1;
    printf("✅ [数据库] gateway.db 初始化成功，设备影子及传感器历史表已就绪！\n");
    return 0;
}


int is_device_in_db(const char *mac) 
{
    if (!g_db) return 0;
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT 1 FROM devices WHERE mac = '%s';", mac);
    
    sqlite3_stmt *stmt;
    int found = 0;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) 
    {
        if (sqlite3_step(stmt) == SQLITE_ROW) found = 1;
        sqlite3_finalize(stmt); 
    }
    return found;
}

int device_announcement_is_reconnect(const char *mac, uint16_t short_addr)
{
    sqlite3_stmt *stmt = NULL;
    int reconnect = 0;
    if (!g_db || !mac) return 0;
    if (sqlite3_prepare_v2(g_db,
        "SELECT join_confirmed,status,short_addr FROM devices WHERE mac=?;",
        -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0)) {
            const char *status = (const char *)sqlite3_column_text(stmt, 1);
            const char *address = (const char *)sqlite3_column_text(stmt, 2);
            char current[5];
            snprintf(current, sizeof(current), "%04X", (unsigned)short_addr);
            reconnect = !status || sqlite3_stricmp(status, "online") ||
                        !address || sqlite3_stricmp(address, current);
        }
    }
    sqlite3_finalize(stmt);
    return reconnect != 0;
}

int confirm_device_join(const char *mac, uint16_t short_addr)
{
    if (!g_db || !mac || !*mac || !short_addr || short_addr>=0xfff8U) return 0;
    char address[5];
    snprintf(address,sizeof(address),"%04X",(unsigned)short_addr);
    sqlite3_stmt *s=NULL;
    if (sqlite3_prepare_v2(g_db,
        "UPDATE devices SET join_confirmed=1 WHERE mac=? AND short_addr=? COLLATE NOCASE AND join_confirmed=0;",
        -1,&s,NULL)!=SQLITE_OK) return -1;
    sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,2,address,-1,SQLITE_TRANSIENT);
    sqlite3_mutex *mu=sqlite3_db_mutex(g_db);sqlite3_mutex_enter(mu);
    int rc=sqlite3_step(s),result=rc==SQLITE_DONE?(sqlite3_changes(g_db)>0):-1;
    sqlite3_finalize(s);sqlite3_mutex_leave(mu);
    return result;
}

int device_is_authorized(const char *mac)
{
    sqlite3_stmt *s=NULL;int yes=0;
    if(!g_db || !mac || sqlite3_prepare_v2(g_db,
        "SELECT user_authorized FROM devices WHERE mac=?;",-1,&s,NULL)!=SQLITE_OK)return 0;
    sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT);
    if(sqlite3_step(s)==SQLITE_ROW)yes=sqlite3_column_int(s,0)!=0;
    sqlite3_finalize(s);return yes;
}

int device_admission_ready(const char *mac)
{
    sqlite3_stmt *s=NULL;int yes=0;
    if(!g_db || !mac || sqlite3_prepare_v2(g_db,
        "SELECT 1 FROM devices WHERE mac=? AND join_confirmed=1 AND user_authorized=0 "
        "AND name NOT IN ('','Unknown','Unknown Device') AND lqi>=0 AND rssi>-128 AND link_updated_at>0 "
        "AND EXISTS(SELECT 1 FROM node_interview i WHERE i.mac=devices.mac AND i.json LIKE '%\"state\":\"ready\"%');",
        -1,&s,NULL)!=SQLITE_OK)return 0;
    sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT);yes=sqlite3_step(s)==SQLITE_ROW;
    sqlite3_finalize(s);return yes;
}

int device_admission_requested(const char *mac)
{
    sqlite3_stmt *s=NULL;
    if(!g_db || !mac || sqlite3_prepare_v2(g_db,
        "SELECT 1 FROM devices WHERE mac=? AND sampling_admission=1;",-1,&s,NULL)!=SQLITE_OK)return 0;
    sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT);
    int requested=sqlite3_step(s)==SQLITE_ROW;
    sqlite3_finalize(s);
    return requested;
}

int request_device_admission(const char *mac)
{
    if(device_is_authorized(mac) || device_admission_requested(mac))return 0;
    if(!device_admission_ready(mac))return -2;
    sqlite3_stmt *s=NULL;
    if(sqlite3_prepare_v2(g_db,
        "UPDATE devices SET sampling_admission=1,monitor_policy_id=1,raw_upload_generation=1,"
        "expected_interval_sec=300 WHERE mac=? AND user_authorized=0 AND sampling_admission=0 AND join_confirmed=1;",
        -1,&s,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT);
    sqlite3_mutex *mu=sqlite3_db_mutex(g_db);sqlite3_mutex_enter(mu);
    int rc=sqlite3_step(s),changed=sqlite3_changes(g_db);
    sqlite3_finalize(s);sqlite3_mutex_leave(mu);
    return rc!=SQLITE_DONE?-1:changed?1:device_admission_requested(mac)?0:-2;
}

int authorize_device(const char *mac)
{
    if(device_is_authorized(mac))return 0;
    sqlite3_stmt *s=NULL;
    if(sqlite3_prepare_v2(g_db,
        "UPDATE devices SET user_authorized=1,monitoring_enabled=1,"
        "sampling_admission=CASE WHEN sampling_admission=1 THEN 2 ELSE sampling_admission END "
        "WHERE mac=? AND user_authorized=0 "
        "AND join_confirmed=1 AND name NOT IN ('','Unknown','Unknown Device') "
        "AND lqi>=0 AND rssi>-128 AND link_updated_at>0 "
        "AND EXISTS(SELECT 1 FROM node_interview i WHERE i.mac=devices.mac AND i.json LIKE '%\"state\":\"ready\"%');",
        -1,&s,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT);
    sqlite3_mutex *mu=sqlite3_db_mutex(g_db);sqlite3_mutex_enter(mu);
    int rc=sqlite3_step(s),changed=sqlite3_changes(g_db);
    sqlite3_finalize(s);sqlite3_mutex_leave(mu);
    return rc!=SQLITE_DONE?-1:changed?1:device_is_authorized(mac)?0:-2;
}


int save_device_interview(const char *mac,const char *json)
{
    sqlite3_stmt *s=NULL;
    if(!g_db || !mac || !json || strlen(json)>32768) return -1;
    if(sqlite3_prepare_v2(g_db,"INSERT INTO node_interview(mac,json) VALUES(?,?) ON CONFLICT(mac) DO UPDATE SET json=excluded.json",-1,&s,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,2,json,-1,SQLITE_TRANSIENT);
    int rc=sqlite3_step(s); sqlite3_finalize(s); return rc==SQLITE_DONE?0:-1;
}
char *load_device_interview(const char *mac)
{
    sqlite3_stmt *s=NULL; char *json=NULL;
    if(!g_db || sqlite3_prepare_v2(g_db,"SELECT json FROM node_interview WHERE mac=?",-1,&s,NULL)!=SQLITE_OK)return NULL;
    sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT);
    if(sqlite3_step(s)==SQLITE_ROW) {const char *p=(const char *)sqlite3_column_text(s,0); if(p)json=strdup(p);}
    sqlite3_finalize(s);return json;
}
int insert_new_device(const char *mac, const char *model_name)
{
    if (!g_db) return -1;

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT OR IGNORE INTO devices "
        "(name, custom_name, mac, short_addr, status, work_mode, monitoring_enabled, collection_state, expected_interval_sec) "
        "VALUES (?, NULL, ?, 'UNKNOWN', 'waiting', 'sleep', 0, 'idle', "
        "COALESCE((SELECT auto_interval_sec FROM monitor_config WHERE id=1),300));";

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 准备插入设备失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, model_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, mac, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        fprintf(stderr, "❌ [数据库] 插入设备失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    return 0;
}


int update_device_online(const char *mac, const char *short_addr)
{
    if (!g_db || !mac || !short_addr || strlen(short_addr)!=4) return -1;
    for (unsigned i=0;i<4;i++) if(!isxdigit((unsigned char)short_addr[i])) return -1;
    /* One atomic statement: a short address belongs to only one IEEE at a
     * time. Preserve displaced devices and their data, but stop routing to
     * their stale address. An unknown MAC must not evict another mapping. */
    const char *sql =
        "UPDATE devices SET "
        "short_addr=CASE WHEN mac=?1 THEN UPPER(?2) ELSE NULL END,"
        "status=CASE WHEN mac=?1 THEN 'online' ELSE 'offline' END,"
        "last_active=CASE WHEN mac=?1 THEN CAST(strftime('%s','now') AS INTEGER) ELSE last_active END "
        "WHERE mac=?1 OR (short_addr=?2 COLLATE NOCASE AND EXISTS(SELECT 1 FROM devices WHERE mac=?1));";
    sqlite3_stmt *stmt=NULL;
    if(sqlite3_prepare_v2(g_db,sql,-1,&stmt,NULL)!=SQLITE_OK) return -1;
    sqlite3_bind_text(stmt,1,mac,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,2,short_addr,-1,SQLITE_TRANSIENT);
    int rc=sqlite3_step(stmt); sqlite3_finalize(stmt);
    return rc==SQLITE_DONE?0:-1;
}


int update_device_offline(const char *mac)
{
    if (!g_db) return -1;
    char sql[256];
    char *err_msg = 0;
    
    // 设备主动离线时，顺手擦除温度和磁力缓存
    snprintf(sql, sizeof(sql), 
             "UPDATE devices SET status = 'offline', temp = '--', mag = '--' WHERE mac = '%s';", 
             mac);
             
    int rc = sqlite3_exec(g_db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) 
    {
        fprintf(stderr, "❌ [数据库] 更新离线状态失败: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }
    return 0;
}


int update_device_model_name(const char *mac, const char *model_name)
{
    if (!g_db) return -1;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "UPDATE devices SET name = ?1 WHERE mac = ?2 AND COALESCE(name,'') <> ?1;";

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 准备更新设备型号失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, model_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, mac, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        fprintf(stderr, "❌ [数据库] 更新设备型号失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    return 0;
}


int update_device_custom_name(const char *mac, const char *custom_name)
{
    if (!g_db) return -1;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "UPDATE devices SET custom_name = ? WHERE mac = ?;";

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 准备更新自定义名称失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    if (custom_name && custom_name[0] != '\0')
    {
        sqlite3_bind_text(stmt, 1, custom_name, -1, SQLITE_TRANSIENT);
    }
    else
    {
        sqlite3_bind_null(stmt, 1);
    }

    sqlite3_bind_text(stmt, 2, mac, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);

    if (rc == SQLITE_CONSTRAINT)
    {
        sqlite3_finalize(stmt);
        return 1;
    }

    if (rc != SQLITE_DONE)
    {
        fprintf(stderr, "❌ [数据库] 更新自定义名称失败: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -1;
    }

    int changed = sqlite3_changes(g_db);
    sqlite3_finalize(stmt);
    return changed > 0 ? 0 : 2;
}


int get_device_name_info(const char *mac,
                         char *model_name,
                         size_t model_name_size,
                         char *custom_name,
                         size_t custom_name_size,
                         char *display_name,
                         size_t display_name_size)
{
    if (!g_db || !mac) return 0;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT name, custom_name FROM devices WHERE mac = ? LIMIT 1;";

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        return 0;
    }

    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        return 0;
    }

    const char *db_model = (const char *)sqlite3_column_text(stmt, 0);
    const char *db_custom = (const char *)sqlite3_column_text(stmt, 1);
    const char *safe_model = db_model ? db_model : "Unknown Device";
    const char *safe_custom = db_custom ? db_custom : "";

    if (model_name && model_name_size > 0)
    {
        snprintf(model_name, model_name_size, "%s", safe_model);
    }

    if (custom_name && custom_name_size > 0)
    {
        snprintf(custom_name, custom_name_size, "%s", safe_custom);
    }

    if (display_name && display_name_size > 0)
    {
        if (safe_custom[0] != '\0')
        {
            snprintf(display_name, display_name_size, "%s", safe_custom);
        }
        else
        {
            build_default_device_name(safe_model, mac, display_name, display_name_size);
        }
    }

    sqlite3_finalize(stmt);
    return 1;
}


// 更新设备工作模式
int update_device_work_mode(const char *mac, const char *work_mode)
{
    if (!g_db) return -1;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "UPDATE devices SET work_mode = ? WHERE mac = ?;";

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 准备更新设备工作模式失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, work_mode, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, mac, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE)
    {
        fprintf(stderr, "❌ [数据库] 更新设备工作模式失败: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -1;
    }

    sqlite3_finalize(stmt);
    return 0;
}


int delete_device(const char *mac) 
{
    static const char *delete_sql[] = {
        "DELETE FROM sampling_resume_anchor WHERE mac=?;",
        "DELETE FROM collection_capture_times WHERE mac=?;",
        "DELETE FROM collection_values WHERE mac=?;",
        "DELETE FROM vibration_batches WHERE mac=?;",
        "DELETE FROM temperature_data WHERE mac=?;",
        "DELETE FROM magnetic_data WHERE mac=?;",
        "DELETE FROM sound_data WHERE mac=?;",
        "DELETE FROM monitor_policies WHERE owner_mac=?;",
        "DELETE FROM devices WHERE mac=?;"
    };
    sqlite3_stmt *stmt = NULL;
    char *err_msg = NULL;

    if (!g_db || !mac || mac[0] == '\0') return -1;

    if (sqlite3_exec(g_db, "BEGIN IMMEDIATE;", NULL, NULL, &err_msg) != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库错误] 无法开始设备删除事务: %s\n",
                err_msg ? err_msg : sqlite3_errmsg(g_db));
        sqlite3_free(err_msg);
        return -1;
    }

    for (size_t i = 0; i < sizeof(delete_sql) / sizeof(delete_sql[0]); ++i)
    {
        if (sqlite3_prepare_v2(g_db, delete_sql[i], -1, &stmt, NULL) != SQLITE_OK)
            goto rollback;
        sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE)
        {
            sqlite3_finalize(stmt);
            stmt = NULL;
            goto rollback;
        }
        sqlite3_finalize(stmt);
        stmt = NULL;
    }

    if (sqlite3_exec(g_db, "COMMIT;", NULL, NULL, &err_msg) == SQLITE_OK)
        return 0;

    fprintf(stderr, "❌ [数据库错误] 提交设备删除事务失败: %s\n",
            err_msg ? err_msg : sqlite3_errmsg(g_db));
    sqlite3_free(err_msg);
    sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
    return -1;

rollback:
    if (stmt) sqlite3_finalize(stmt);
    fprintf(stderr, "❌ [数据库错误] 删除设备相关数据失败: %s\n",
            sqlite3_errmsg(g_db));
    sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
}

static int revoke_device_data(const char *mac, int short_addr, int unapproved_only)
{
    static const char *delete_sql[] = {
        "DELETE FROM sampling_resume_anchor WHERE mac=?;",
        "DELETE FROM collection_capture_times WHERE mac=?;",
        "DELETE FROM collection_values WHERE mac=?;",
        "DELETE FROM vibration_batches WHERE mac=?;",
        "DELETE FROM temperature_data WHERE mac=?;",
        "DELETE FROM magnetic_data WHERE mac=?;",
        "DELETE FROM sound_data WHERE mac=?;",
        "DELETE FROM monitor_policies WHERE owner_mac=?;",
        "DELETE FROM devices WHERE mac=?;"
    };
    sqlite3_stmt *stmt = NULL;
    char *err_msg = NULL;

    if (!g_db || !mac || mac[0] == '\0') return -1;
    sqlite3_mutex *mu=sqlite3_db_mutex(g_db);sqlite3_mutex_enter(mu);
    if (sqlite3_exec(g_db, "BEGIN IMMEDIATE;", NULL, NULL, &err_msg) != SQLITE_OK)
        goto fail;

    if(unapproved_only) {
        if(sqlite3_prepare_v2(g_db,"SELECT user_authorized FROM devices WHERE mac=?;",-1,&stmt,NULL)!=SQLITE_OK)
            goto rollback_revoke;
        sqlite3_bind_text(stmt,1,mac,-1,SQLITE_TRANSIENT);
        int step=sqlite3_step(stmt),skip=step==SQLITE_DONE || (step==SQLITE_ROW && sqlite3_column_int(stmt,0));
        sqlite3_finalize(stmt);stmt=NULL;
        if(step!=SQLITE_DONE && step!=SQLITE_ROW)goto rollback_revoke;
        if(skip) {
            (void)sqlite3_exec(g_db,"ROLLBACK;",NULL,NULL,NULL);
            sqlite3_mutex_leave(mu);return 1;
        }
    }

    const char *revoke_sql =
        "INSERT INTO revoked_nodes"
        "(mac,last_short_addr,state,deleted_at,last_seen_at,last_leave_at,retry_count) "
        "VALUES(?,?,'pending',strftime('%s','now'),strftime('%s','now'),0,0) "
        "ON CONFLICT(mac) DO UPDATE SET "
        "last_short_addr=excluded.last_short_addr,state='pending',"
        "deleted_at=excluded.deleted_at,last_seen_at=excluded.last_seen_at,"
        "last_leave_at=0,retry_count=0;";
    if (sqlite3_prepare_v2(g_db, revoke_sql, -1, &stmt, NULL) != SQLITE_OK)
        goto rollback_revoke;
    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    if (short_addr >= 0 && short_addr <= 0xFFFF)
        sqlite3_bind_int(stmt, 2, short_addr);
    else
        sqlite3_bind_null(stmt, 2);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto rollback_revoke;
    sqlite3_finalize(stmt);
    stmt = NULL;

    for (size_t i = 0; i < sizeof(delete_sql) / sizeof(delete_sql[0]); ++i)
    {
        if (sqlite3_prepare_v2(g_db, delete_sql[i], -1, &stmt, NULL) != SQLITE_OK)
            goto rollback_revoke;
        sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) goto rollback_revoke;
        sqlite3_finalize(stmt);
        stmt = NULL;
    }

    if (sqlite3_exec(g_db, "COMMIT;", NULL, NULL, &err_msg) == SQLITE_OK) {
        sqlite3_mutex_leave(mu);return 0;
    }

rollback_revoke:
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
fail:
    fprintf(stderr, "❌ [数据库错误] 创建设备撤销记录失败: %s\n",
            err_msg ? err_msg : sqlite3_errmsg(g_db));
    sqlite3_free(err_msg);
    sqlite3_mutex_leave(mu);
    return -1;
}

int revoke_device_and_delete_data(const char *mac, int short_addr)
{return revoke_device_data(mac,short_addr,0);}
int cancel_unapproved_device(const char *mac, int short_addr)
{return revoke_device_data(mac,short_addr,1);}

int queue_revoked_node(const char *mac, uint16_t short_addr)
{
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (!g_db || !mac || mac[0] == '\0') return -1;
    if (sqlite3_prepare_v2(g_db,
        "INSERT INTO revoked_nodes"
        "(mac,last_short_addr,state,deleted_at,last_seen_at,last_leave_at,retry_count) "
        "VALUES(?,?,'pending',strftime('%s','now'),strftime('%s','now'),0,0) "
        "ON CONFLICT(mac) DO UPDATE SET "
        "last_short_addr=excluded.last_short_addr,state='pending',"
        "last_seen_at=excluded.last_seen_at;",
        -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, short_addr);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int read_revoked_node(sqlite3_stmt *stmt, RevokedNodeRecord_t *record)
{
    if (sqlite3_step(stmt) != SQLITE_ROW) return 0;
    if (record)
    {
        const unsigned char *mac = sqlite3_column_text(stmt, 0);
        const unsigned char *state = sqlite3_column_text(stmt, 2);
        memset(record, 0, sizeof(*record));
        snprintf(record->mac, sizeof(record->mac), "%s",
                 mac ? (const char *)mac : "");
        record->has_short_addr = sqlite3_column_type(stmt, 1) != SQLITE_NULL;
        record->last_short_addr = (uint16_t)sqlite3_column_int(stmt, 1);
        snprintf(record->state, sizeof(record->state), "%s",
                 state ? (const char *)state : "pending");
        record->deleted_at = sqlite3_column_int64(stmt, 3);
        record->last_seen_at = sqlite3_column_int64(stmt, 4);
        record->last_leave_at = sqlite3_column_int64(stmt, 5);
        record->retry_count = (unsigned int)sqlite3_column_int(stmt, 6);
    }
    return 1;
}

int get_revoked_node_by_mac(const char *mac, RevokedNodeRecord_t *record)
{
    sqlite3_stmt *stmt = NULL;
    int found = 0;
    if (!g_db || !mac) return 0;
    if (sqlite3_prepare_v2(g_db,
        "SELECT mac,last_short_addr,state,deleted_at,last_seen_at,last_leave_at,retry_count "
        "FROM revoked_nodes WHERE mac=? COLLATE NOCASE;", -1, &stmt, NULL) == SQLITE_OK)
    {
        sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
        found = read_revoked_node(stmt, record);
    }
    sqlite3_finalize(stmt);
    return found;
}

int get_revoked_node_by_short(uint16_t short_addr, RevokedNodeRecord_t *record)
{
    sqlite3_stmt *stmt = NULL;
    int found = 0;
    if (!g_db) return 0;
    if (sqlite3_prepare_v2(g_db,
        "SELECT mac,last_short_addr,state,deleted_at,last_seen_at,last_leave_at,retry_count "
        "FROM revoked_nodes WHERE last_short_addr=? LIMIT 1;", -1, &stmt, NULL) == SQLITE_OK)
    {
        sqlite3_bind_int(stmt, 1, short_addr);
        found = read_revoked_node(stmt, record);
    }
    sqlite3_finalize(stmt);
    return found;
}

int update_revoked_node_seen(const char *mac, uint16_t short_addr,
                             const char *state)
{
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (!g_db || !mac || !state) return -1;
    if (sqlite3_prepare_v2(g_db,
        "UPDATE revoked_nodes SET last_short_addr=?,state=?,"
        "last_seen_at=strftime('%s','now') WHERE mac=? COLLATE NOCASE;",
        -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, short_addr);
    sqlite3_bind_text(stmt, 2, state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, mac, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int mark_revoked_leave_attempt(const char *mac, uint16_t short_addr)
{
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (!g_db || !mac) return -1;
    if (sqlite3_prepare_v2(g_db,
        "UPDATE revoked_nodes SET last_short_addr=?,state='sending',"
        "last_seen_at=strftime('%s','now'),last_leave_at=strftime('%s','now'),"
        "retry_count=retry_count+1 WHERE mac=? COLLATE NOCASE;",
        -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, short_addr);
    sqlite3_bind_text(stmt, 2, mac, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int mark_revoked_node_guard(const char *mac, uint16_t short_addr)
{
    return update_revoked_node_seen(mac, short_addr, "guard");
}

int clear_revoked_node(const char *mac)
{
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (!g_db || !mac) return -1;
    if (sqlite3_prepare_v2(g_db,
        "DELETE FROM revoked_nodes WHERE mac=? COLLATE NOCASE;",
        -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int release_revoked_short_for_other_mac(const char *mac, uint16_t short_addr)
{
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (!g_db || !mac) return -1;
    if (sqlite3_prepare_v2(g_db,
        "UPDATE revoked_nodes SET last_short_addr=NULL "
        "WHERE last_short_addr=? AND mac<>? COLLATE NOCASE;",
        -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, short_addr);
    sqlite3_bind_text(stmt, 2, mac, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int update_device_expected_interval(const char *mac, uint32_t interval_sec)
{
    sqlite3_stmt *stmt = NULL;
    if (!g_db || !mac || interval_sec == 0U) return -1;
    if (sqlite3_prepare_v2(g_db,
        "UPDATE devices SET expected_interval_sec=? WHERE mac=?;",
        -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)interval_sec);
    sqlite3_bind_text(stmt, 2, mac, -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return ok;
}


// 💡 极其轻量的单表查询，速度提升百倍
int update_pending_probe_telemetry(const char *mac, int lqi, int rssi)
{
    if(!g_db || !mac || lqi<0 || lqi>255 || rssi<=-128 || rssi>127)return -1;
    sqlite3_stmt *stmt=NULL;
    if(sqlite3_prepare_v2(g_db,"UPDATE devices SET lqi=?,rssi=?,link_updated_at=?,"
        "last_active=?,status='online' WHERE mac=? AND user_authorized=0;",-1,&stmt,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_int(stmt,1,lqi);sqlite3_bind_int(stmt,2,rssi);
    sqlite3_int64 now=(sqlite3_int64)time(NULL);
    sqlite3_bind_int64(stmt,3,now);sqlite3_bind_int64(stmt,4,now);
    sqlite3_bind_text(stmt,5,mac,-1,SQLITE_TRANSIENT);
    sqlite3_mutex *mutex=sqlite3_db_mutex(g_db);sqlite3_mutex_enter(mutex);
    int ok=sqlite3_step(stmt)==SQLITE_DONE && sqlite3_changes(g_db)==1;
    sqlite3_mutex_leave(mutex);sqlite3_finalize(stmt);return ok?0:-1;
}

int update_device_link_telemetry(const char *mac, int lqi, int rssi, int power_source)
{
    if (!g_db || !mac || lqi < 0 || lqi > 255 || rssi < -128 || rssi > 127 ||
        power_source < 0 || power_source > 6) return -1;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "UPDATE devices SET lqi=?, rssi=?, power_source=?, link_updated_at=? WHERE mac=?;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, lqi);
    sqlite3_bind_int(stmt, 2, rssi);
    sqlite3_bind_int(stmt, 3, power_source);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)time(NULL));
    sqlite3_bind_text(stmt, 5, mac, -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(g_db) == 1;
    sqlite3_finalize(stmt);
    return ok ? 0 : -1;
}

int update_device_description(const char *mac, const char *description)
{
    if (!g_db || !mac || !description || strlen(description) > 768) return -1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, "UPDATE devices SET description=? WHERE mac=? COLLATE NOCASE;", -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, description, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, mac, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    int changed = sqlite3_changes(g_db);
    sqlite3_finalize(stmt);
    return rc != SQLITE_DONE ? -1 : changed ? 0 : 2;
}

int get_all_devices(DeviceRecord_t **out_records)
{
    if (!g_db) return 0;
    
    int count = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM devices;", -1, &stmt, NULL) == SQLITE_OK) 
    {
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    
    if (count == 0) 
    {
        *out_records = NULL;
        return 0;
    }
    
    *out_records = (DeviceRecord_t *)malloc(sizeof(DeviceRecord_t) * count);
    int idx = 0;
    
    // 直接全表扫描，没有任何 JOIN 和子查询
    const char *sql = "SELECT name, custom_name, mac, short_addr, status, temp, mag, digital_db, analog_db, work_mode, monitoring_enabled, collection_state, last_collection, last_active, expected_interval_sec, monitor_policy_id, raw_upload_generation, lqi, rssi, power_source, link_updated_at, description, collection_batch_id, capture_batch_id, capture_pending, join_confirmed,user_authorized,sampling_admission FROM devices;";
        
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) 
    {
        while (sqlite3_step(stmt) == SQLITE_ROW && idx < count) 
        {
            const char *model_name = (const char *)sqlite3_column_text(stmt, 0);
            snprintf((*out_records)[idx].model_name,
                     sizeof((*out_records)[idx].model_name),
                     "%s",
                     model_name ? model_name : "Unknown Device");

            const char *custom_name = (const char *)sqlite3_column_text(stmt, 1);
            snprintf((*out_records)[idx].custom_name,
                     sizeof((*out_records)[idx].custom_name),
                     "%s",
                     custom_name ? custom_name : "");

            const char *mac = (const char *)sqlite3_column_text(stmt, 2);
            strcpy((*out_records)[idx].mac, mac ? mac : "");

            if ((*out_records)[idx].custom_name[0] != '\0')
            {
                snprintf((*out_records)[idx].name,
                         sizeof((*out_records)[idx].name),
                         "%s",
                         (*out_records)[idx].custom_name);
            }
            else
            {
                build_default_device_name((*out_records)[idx].model_name,
                                          (*out_records)[idx].mac,
                                          (*out_records)[idx].name,
                                          sizeof((*out_records)[idx].name));
            }
            
            const char *s_addr = (const char *)sqlite3_column_text(stmt, 3);
            strcpy((*out_records)[idx].short_addr, s_addr ? s_addr : "UNKNOWN");
            
            const char *stat = (const char *)sqlite3_column_text(stmt, 4);
            strcpy((*out_records)[idx].status, stat ? stat : "offline");
            
            // 直接拷贝现成的文本数据
            const char *t_val = (const char *)sqlite3_column_text(stmt, 5);
            strcpy((*out_records)[idx].temp, t_val ? t_val : "--");
            
            const char *m_val = (const char *)sqlite3_column_text(stmt, 6);
            strcpy((*out_records)[idx].mag, m_val ? m_val : "--");

            const char *digital_db = (const char *)sqlite3_column_text(stmt, 7);
            strcpy((*out_records)[idx].digital_db, digital_db ? digital_db : "--");

            const char *analog_db = (const char *)sqlite3_column_text(stmt, 8);
            strcpy((*out_records)[idx].analog_db, analog_db ? analog_db : "--");

            const char *work_mode = (const char *)sqlite3_column_text(stmt, 9);
            strcpy((*out_records)[idx].work_mode, work_mode ? work_mode : "sleep");
            (*out_records)[idx].monitoring_enabled = sqlite3_column_int(stmt, 10);
            const char *collection_state = (const char *)sqlite3_column_text(stmt, 11);
            strcpy((*out_records)[idx].collection_state,
                   collection_state ? collection_state : "idle");
            (*out_records)[idx].last_collection = sqlite3_column_int64(stmt, 12);
            (*out_records)[idx].last_active = sqlite3_column_int64(stmt, 13);
            (*out_records)[idx].expected_interval_sec =
                (uint32_t)sqlite3_column_int64(stmt, 14);
            (*out_records)[idx].monitor_policy_id = sqlite3_column_int(stmt, 15);
            (*out_records)[idx].raw_upload_generation =
                (uint32_t)sqlite3_column_int64(stmt, 16);
            (*out_records)[idx].lqi = sqlite3_column_int(stmt, 17);
            (*out_records)[idx].rssi = sqlite3_column_int(stmt, 18);
            (*out_records)[idx].power_source = sqlite3_column_int(stmt, 19);
            (*out_records)[idx].link_updated_at = sqlite3_column_int64(stmt, 20);
            (*out_records)[idx].collection_batch_id = (uint32_t)sqlite3_column_int64(stmt,22);
            (*out_records)[idx].capture_batch_id = (uint32_t)sqlite3_column_int64(stmt,23);
            (*out_records)[idx].capture_pending = sqlite3_column_int(stmt,24);
            (*out_records)[idx].join_confirmed = sqlite3_column_int(stmt,25);
            (*out_records)[idx].user_authorized = sqlite3_column_int(stmt,26);
            (*out_records)[idx].sampling_admission = sqlite3_column_int(stmt,27);
            const char *description = (const char *)sqlite3_column_text(stmt, 21);
            snprintf((*out_records)[idx].description, sizeof((*out_records)[idx].description), "%s", description ? description : "");
            
            idx++;
        }
        sqlite3_finalize(stmt);
    }
    return idx;
}


int get_confirmed_devices(DeviceRecord_t **out_records)
{
    int count=get_all_devices(out_records),visible=0;
    if(count<0)return count;
    for(int i=0;i<count;i++) {
        if(!(*out_records)[i].join_confirmed || !(*out_records)[i].user_authorized)continue;
        if(visible!=i)(*out_records)[visible]=(*out_records)[i];
        visible++;
    }
    return visible;
}

int get_mac_by_short_addr(const char *short_addr, char *out_mac) 
{
    if (!g_db) return 0; 
    
    sqlite3_stmt *stmt;
    const char *sql = "SELECT mac FROM devices WHERE short_addr = ? LIMIT 1;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    
    sqlite3_bind_text(stmt, 1, short_addr, -1, SQLITE_TRANSIENT);
    
    int found = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) 
    {
        const unsigned char *db_mac = sqlite3_column_text(stmt, 0);
        if (db_mac) 
        {
            strcpy(out_mac, (const char *)db_mac);
            found = 1;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}


// 💡 写入温度：更新设备表最新温度，同时保存一条历史记录
int insert_sensor_temp(const char *mac, float temperature)
{
    if (!g_db) return -1;
    char sql[256];
    char *err_msg = 0;
    
    // 💡 收到温度心跳时，刷新 last_active 时间戳
    snprintf(sql, sizeof(sql),
             "UPDATE devices SET temp = '%.2f', last_active = CAST(strftime('%%s','now') AS INTEGER) WHERE mac = '%s';",
             temperature, mac);

    int rc = sqlite3_exec(g_db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) 
    {
        fprintf(stderr, "❌ [数据库] 刷新设备最新温度失败: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    if (!history_time_is_valid()) return 0;

    sqlite3_stmt *stmt = NULL;

    const char *sql_insert =
        "INSERT INTO temperature_data (mac, temperature, timestamp) "
        "VALUES (?, ?, CAST(strftime('%s','now') AS INTEGER));";

    if (sqlite3_prepare_v2(g_db, sql_insert, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 准备写入温度历史失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, temperature);

    rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE)
    {
        fprintf(stderr, "❌ [数据库] 写入温度历史失败: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -1;
    }

    sqlite3_finalize(stmt);
    return 0;
}


// 💡 写入磁力计：更新设备表最新磁场，同时保存一条历史记录
int insert_sensor_mag(const char *mac, float mag_x, float mag_y, float mag_z)
{
    if (!g_db) return -1;
    char sql[256];
    char *err_msg = 0;
    
    // 💡 收到磁力心跳时，刷新 last_active 时间戳
    snprintf(sql, sizeof(sql),
             "UPDATE devices SET mag = 'X:%.1f, Y:%.1f, Z:%.1f', last_active = CAST(strftime('%%s','now') AS INTEGER) WHERE mac = '%s';",
             mag_x, mag_y, mag_z, mac);

    int rc = sqlite3_exec(g_db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) 
    {
        fprintf(stderr, "❌ [数据库] 刷新设备最新磁力失败: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    if (!history_time_is_valid()) return 0;

    sqlite3_stmt *stmt = NULL;

    const char *sql_insert =
        "INSERT INTO magnetic_data (mac, mag_x, mag_y, mag_z, timestamp) "
        "VALUES (?, ?, ?, ?, CAST(strftime('%s','now') AS INTEGER));";

    if (sqlite3_prepare_v2(g_db, sql_insert, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 准备写入磁场历史失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, mag_x);
    sqlite3_bind_double(stmt, 3, mag_y);
    sqlite3_bind_double(stmt, 4, mag_z);

    rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE)
    {
        fprintf(stderr, "❌ [数据库] 写入磁场历史失败: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -1;
    }

    sqlite3_finalize(stmt);
    return 0;
}


// 写入数字或超声麦克风的最新一秒声级，同时保留带时间戳的历史记录。
int insert_sensor_sound(const char *mac, int is_digital, float db_spl)
{
    if (!g_db || !mac) return -1;

    sqlite3_stmt *stmt = NULL;
    const char *sql = is_digital
        ? "UPDATE devices SET digital_db = printf('%.2f', ?), last_active = CAST(strftime('%s','now') AS INTEGER) WHERE mac = ?;"
        : "UPDATE devices SET analog_db = printf('%.2f', ?), last_active = CAST(strftime('%s','now') AS INTEGER) WHERE mac = ?;";

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 准备写入麦克风声级失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_double(stmt, 1, db_spl);
    sqlite3_bind_text(stmt, 2, mac, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        fprintf(stderr, "❌ [数据库] 写入麦克风声级失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    if (!history_time_is_valid()) return 0;

    const char *sql_insert =
        "INSERT INTO sound_data (mac, source, db_spl, timestamp) "
        "VALUES (?, ?, ?, CAST(strftime('%s','now') AS INTEGER));";

    if (sqlite3_prepare_v2(g_db, sql_insert, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 准备写入麦克风历史失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, is_digital ? 1 : 0);
    sqlite3_bind_double(stmt, 3, db_spl);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        fprintf(stderr, "❌ [数据库] 写入麦克风历史失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    return 0;
}

static int upsert_collection_scalar(const char *mac, uint32_t collection_id,
                                    const char *column, float value)
{
    char sql[384];
    sqlite3_stmt *stmt = NULL;
    if (!g_db || !mac || !column) return -1;
    snprintf(sql, sizeof(sql),
        "INSERT INTO collection_values(mac,batch_id,%s) VALUES(?,?,?) "
        "ON CONFLICT(mac,batch_id) DO UPDATE SET %s=excluded.%s,"
        "timestamp=CAST(strftime('%%s','now') AS INTEGER);",
        column, column, column);
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)collection_id);
    sqlite3_bind_double(stmt, 3, value);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int upsert_collection_temp(const char *mac, uint32_t collection_id, float value)
{
    return upsert_collection_scalar(mac, collection_id, "temperature", value);
}

int upsert_collection_sound(const char *mac, uint32_t collection_id,
                            int is_digital, float value)
{
    return upsert_collection_scalar(mac, collection_id,
                                    is_digital ? "digital_db" : "analog_db", value);
}

int upsert_collection_mag(const char *mac, uint32_t collection_id,
                          float x, float y, float z)
{
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO collection_values(mac,batch_id,mag_x,mag_y,mag_z) VALUES(?,?,?,?,?) "
        "ON CONFLICT(mac,batch_id) DO UPDATE SET mag_x=excluded.mag_x,"
        "mag_y=excluded.mag_y,mag_z=excluded.mag_z,"
        "timestamp=CAST(strftime('%s','now') AS INTEGER);";
    if (!g_db || !mac || sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)collection_id);
    sqlite3_bind_double(stmt, 3, x); sqlite3_bind_double(stmt, 4, y);
    sqlite3_bind_double(stmt, 5, z);
    int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int get_collection_values(const char *mac, uint32_t collection_id,
                          float *temperature, float *mag_x, float *mag_y,
                          float *mag_z, float *digital_db, float *analog_db,
                          unsigned int *present_mask)
{
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT temperature,mag_x,mag_y,mag_z,digital_db,analog_db "
                      "FROM collection_values WHERE mac=? AND batch_id=?;";
    if (!g_db || !mac || !present_mask ||
        sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)collection_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return 0; }
    float *values[6] = {temperature, mag_x, mag_y, mag_z, digital_db, analog_db};
    *present_mask = 0U;
    for (int i = 0; i < 6; i++)
    {
        if (sqlite3_column_type(stmt, i) != SQLITE_NULL)
        {
            if (values[i]) *values[i] = (float)sqlite3_column_double(stmt, i);
            *present_mask |= 1U << i;
        }
    }
    sqlite3_finalize(stmt);
    return 1;
}

static int record_collection_value(const char *mac,uint32_t batch,unsigned field,
                                    float x,float y,float z)
{
    if(!g_db || !mac || field>5 || field==2 || field==3)return -1;
    sqlite3_mutex *mutex=sqlite3_db_mutex(g_db);
    sqlite3_mutex_enter(mutex);
    int result=-1;
    if(sqlite3_exec(g_db,"SAVEPOINT collection_ingest;",NULL,NULL,NULL)!=SQLITE_OK)goto done;
    if(!is_device_in_db(mac))goto rollback;
    float previous[6]={0};unsigned present=0;
    int found=get_collection_values(mac,batch,&previous[0],&previous[1],&previous[2],
                                    &previous[3],&previous[4],&previous[5],&present);
    unsigned mask=field==1 ? 0x0EU : 1U<<field;
    if(found && (present&mask)==mask && previous[field]==x &&
       (field!=1 || (previous[2]==y && previous[3]==z))) {
        result=0;
    } else {
        int stored=field==0 ? upsert_collection_temp(mac,batch,x) :
                   field==1 ? upsert_collection_mag(mac,batch,x,y,z) :
                              upsert_collection_sound(mac,batch,field==4,x);
        if(stored!=0)goto rollback;
        stored=field==0 ? insert_sensor_temp(mac,x) :
               field==1 ? insert_sensor_mag(mac,x,y,z) : insert_sensor_sound(mac,field==4,x);
        if(stored!=0)goto rollback;
        result=1;
    }
    if(sqlite3_exec(g_db,"RELEASE collection_ingest;",NULL,NULL,NULL)==SQLITE_OK)goto done;
rollback:
    sqlite3_exec(g_db,"ROLLBACK TO collection_ingest;",NULL,NULL,NULL);
    sqlite3_exec(g_db,"RELEASE collection_ingest;",NULL,NULL,NULL);
    result=-1;
done:
    sqlite3_mutex_leave(mutex);
    return result;
}
int record_collection_temp(const char *mac,uint32_t batch,float value) {
    return record_collection_value(mac,batch,0,value,0,0);
}
int record_collection_mag(const char *mac,uint32_t batch,float x,float y,float z) {
    return record_collection_value(mac,batch,1,x,y,z);
}
int record_collection_sound(const char *mac,uint32_t batch,int is_digital,float value) {
    return record_collection_value(mac,batch,is_digital?4:5,value,0,0);
}

int get_collection_history(const char *mac, const char *metric, int limit,
                           CollectionHistoryRecord_t **out_records)
{
    if (!out_records) return -1;
    *out_records = NULL;
    if (!g_db || !mac || !metric || limit < 1) return -1;
    if (limit > COLLECTION_HISTORY_LIMIT) limit = COLLECTION_HISTORY_LIMIT;
    const char *condition = !strcmp(metric, "temperature") ? "temperature IS NOT NULL"
        : !strcmp(metric, "digital") ? "digital_db IS NOT NULL"
        : !strcmp(metric, "ultrasonic") ? "analog_db IS NOT NULL"
        : !strcmp(metric, "magnetic") ? "mag_x IS NOT NULL AND mag_y IS NOT NULL AND mag_z IS NOT NULL"
        : NULL;
    if (!condition) return -1;
    char sql[512];
    /* collection_values has PRIMARY KEY(mac,batch_id), so retries cannot add
     * extra points. Limit in SQL, not after loading all historical samples. */
    snprintf(sql, sizeof(sql),
        "SELECT v.batch_id,v.timestamp,temperature,mag_x,mag_y,mag_z,digital_db,analog_db,c.captured_at "
        "FROM collection_values v LEFT JOIN collection_capture_times c "
        "ON c.mac=v.mac AND c.batch_id=v.batch_id WHERE v.mac=? AND %s "
        "ORDER BY COALESCE(c.captured_at,v.timestamp) DESC,v.rowid DESC LIMIT ?;", condition);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    CollectionHistoryRecord_t *records = calloc((size_t)limit, sizeof(*records));
    if (!records) { sqlite3_finalize(stmt); return -1; }
    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    int count = 0, rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        CollectionHistoryRecord_t *record = &records[count++];
        record->batch_id = (uint32_t)sqlite3_column_int64(stmt, 0);
        record->timestamp = sqlite3_column_int64(stmt, 1);
        record->captured_at = sqlite3_column_int64(stmt, 8);
        for (int i = 0; i < 6; ++i) {
            if (sqlite3_column_type(stmt, i+2) != SQLITE_NULL) {
                record->values[i] = (float)sqlite3_column_double(stmt, i+2);
                record->present_mask |= 1U << i;
            }
        }
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) { free(records); return -1; }
    for (int i = 0; i < count/2; ++i) {
        CollectionHistoryRecord_t swap = records[i];
        records[i] = records[count-1-i]; records[count-1-i] = swap;
    }
    if (!count) { free(records); records = NULL; }
    *out_records = records;
    return count;
}

int update_device_monitoring(const char *mac, int enabled)
{
    sqlite3_stmt *stmt = NULL;
    if (!g_db || !mac) return -1;
    if (sqlite3_prepare_v2(g_db,
        "UPDATE devices SET monitoring_enabled=? WHERE mac=?;", -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
    sqlite3_bind_text(stmt, 2, mac, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int get_device_monitoring(const char *mac, int *enabled)
{
    sqlite3_stmt *stmt = NULL;
    if (!g_db || !mac || !enabled) return 0;
    if (sqlite3_prepare_v2(g_db,
        "SELECT monitoring_enabled FROM devices WHERE mac=?;",
        -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    int found = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        *enabled = sqlite3_column_int(stmt, 0) ? 1 : 0;
        found = 1;
    }
    sqlite3_finalize(stmt);
    return found;
}

int update_device_collection_state(const char *mac, const char *state, int mark_completed)
{
    return update_device_collection_batch_state(mac,state,mark_completed,0);
}

int update_device_collection_batch_state(const char *mac,const char *state,int mark_completed,uint32_t batch)
{
    sqlite3_stmt *stmt = NULL;
    const char *sql = "UPDATE devices SET collection_state=?1,"
        "collection_batch_id=CASE WHEN ?3>0 THEN ?3 ELSE collection_batch_id END,"
        "capture_batch_id=CASE WHEN ?1='capturing' AND ?3>0 THEN ?3 ELSE capture_batch_id END,"
        "capture_pending=CASE WHEN ?1='capturing' AND ?3>0 THEN 1 WHEN ?4 AND ?3=capture_batch_id THEN 0 ELSE capture_pending END,"
        "last_collection=CASE WHEN ?4 THEN CAST(strftime('%s','now') AS INTEGER) ELSE last_collection END WHERE mac=?2;";
    if (!g_db || !mac || !state) return -1;
    if (batch && !strcmp(state, "capturing")) {
        /* Retries and delayed duplicates must retain the first observation. */
        if (sqlite3_prepare_v2(g_db,
            "INSERT OR IGNORE INTO collection_capture_times(mac,batch_id,captured_at) "
            "SELECT mac,?,CAST(strftime('%s','now') AS INTEGER) FROM devices WHERE mac=?;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
        sqlite3_bind_int64(stmt, 1, batch);
        sqlite3_bind_text(stmt, 2, mac, -1, SQLITE_TRANSIENT);
        int capture_rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        stmt = NULL;
        if (capture_rc != SQLITE_DONE) return -1;
    }
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, mac, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt,3,batch);sqlite3_bind_int(stmt,4,mark_completed);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

long long get_collection_capture_time(const char *mac, uint32_t batch)
{
    sqlite3_stmt *stmt = NULL;
    long long captured_at = 0;
    if (!g_db || !mac || !batch) return 0;
    if (sqlite3_prepare_v2(g_db,
        "SELECT captured_at FROM collection_capture_times WHERE mac=? AND batch_id=?;",
        -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, batch);
        if (sqlite3_step(stmt) == SQLITE_ROW) captured_at = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return captured_at;
}

int get_sampling_resume_plan(const char *mac, uint32_t period_sec, long long now_ms,
                             uint32_t *deadline_sec, uint32_t *delay_ms)
{
    sqlite3_stmt *stmt = NULL;
    long long anchor = 0;
    if (!g_db || !mac || !deadline_sec || !delay_ms || now_ms<=0 ||
        period_sec<MONITOR_MIN_AUTO_INTERVAL_SEC || period_sec>2147483U) return 0;
    if (sqlite3_prepare_v2(g_db,
        "SELECT COALESCE(MAX(captured_at),0)*1000 FROM collection_capture_times WHERE mac=?;",
        -1,&stmt,NULL)!=SQLITE_OK) return -1;
    sqlite3_bind_text(stmt,1,mac,-1,SQLITE_TRANSIENT);
    int rc=sqlite3_step(stmt);
    if (rc==SQLITE_ROW) anchor=sqlite3_column_int64(stmt,0);
    sqlite3_finalize(stmt);stmt=NULL;
    if (rc!=SQLITE_ROW) return -1;
    if (!anchor) {
        if (sqlite3_prepare_v2(g_db,
            "INSERT OR IGNORE INTO sampling_resume_anchor(mac,anchor_ms) SELECT mac,? FROM devices WHERE mac=?;",
            -1,&stmt,NULL)!=SQLITE_OK) return -1;
        sqlite3_bind_int64(stmt,1,now_ms);sqlite3_bind_text(stmt,2,mac,-1,SQLITE_TRANSIENT);
        rc=sqlite3_step(stmt);sqlite3_finalize(stmt);stmt=NULL;
        if (rc!=SQLITE_DONE) return -1;
        if (sqlite3_prepare_v2(g_db,"SELECT anchor_ms FROM sampling_resume_anchor WHERE mac=?;",
            -1,&stmt,NULL)!=SQLITE_OK) return -1;
        sqlite3_bind_text(stmt,1,mac,-1,SQLITE_TRANSIENT);
        rc=sqlite3_step(stmt);
        if (rc==SQLITE_ROW) anchor=sqlite3_column_int64(stmt,0);
        sqlite3_finalize(stmt);
        if (rc!=SQLITE_ROW) return rc==SQLITE_DONE?0:-1;
    }
    long long period=(long long)period_sec*1000;
    long long phase=(now_ms-anchor)%period;
    if (phase<0) phase+=period;
    long long delay=period-phase,deadline=(now_ms+delay)/1000;
    if (deadline<=0 || deadline>UINT32_MAX) return 0;
    *deadline_sec=(uint32_t)deadline;*delay_ms=(uint32_t)delay;
    return 1;
}

int get_device_short_addr(const char *mac, uint16_t *out_short_addr)
{
    sqlite3_stmt *stmt = NULL;
    if (!g_db || !mac || !out_short_addr) return 0;
    if (sqlite3_prepare_v2(g_db, "SELECT short_addr FROM devices WHERE mac=?;", -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    int found = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *value = (const char *)sqlite3_column_text(stmt, 0);
        unsigned int parsed = 0;
        if (value && sscanf(value, "%x", &parsed) == 1)
        {
            *out_short_addr = (uint16_t)parsed;
            found = 1;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

int save_monitor_config(uint32_t auto_interval_sec, uint16_t poll_interval_sec,
                        uint16_t vibration_rate_hz, uint16_t digital_mic_rate_hz,
                        uint32_t analog_mic_rate_hz, uint16_t vibration_window_ms,
                        uint16_t digital_mic_window_ms, uint16_t analog_mic_window_ms,
                        uint16_t post_upload_rx_window_sec)
{
    sqlite3_stmt *stmt = NULL;
    const char *sql = "UPDATE monitor_config SET auto_interval_sec=?,poll_interval_sec=?,vibration_rate_hz=?,digital_mic_rate_hz=?,analog_mic_rate_hz=?,mic_window_ms=?,vibration_window_ms=?,digital_mic_window_ms=?,analog_mic_window_ms=?,post_upload_rx_window_sec=? WHERE id=1;";
    if (!g_db || sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, auto_interval_sec);
    sqlite3_bind_int(stmt, 2, poll_interval_sec);
    sqlite3_bind_int(stmt, 3, vibration_rate_hz);
    sqlite3_bind_int(stmt, 4, digital_mic_rate_hz);
    sqlite3_bind_int64(stmt, 5, analog_mic_rate_hz);
    sqlite3_bind_int(stmt, 6, digital_mic_window_ms);
    sqlite3_bind_int(stmt, 7, vibration_window_ms);
    sqlite3_bind_int(stmt, 8, digital_mic_window_ms);
    sqlite3_bind_int(stmt, 9, analog_mic_window_ms);
    sqlite3_bind_int(stmt, 10, post_upload_rx_window_sec);
    int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int load_monitor_config(uint32_t *auto_interval_sec, uint16_t *poll_interval_sec,
                        uint16_t *vibration_rate_hz, uint16_t *digital_mic_rate_hz,
                        uint32_t *analog_mic_rate_hz, uint16_t *vibration_window_ms,
                        uint16_t *digital_mic_window_ms, uint16_t *analog_mic_window_ms,
                        uint16_t *post_upload_rx_window_sec)
{
    sqlite3_stmt *stmt = NULL;
    if (!g_db || sqlite3_prepare_v2(g_db, "SELECT auto_interval_sec,poll_interval_sec,vibration_rate_hz,digital_mic_rate_hz,analog_mic_rate_hz,vibration_window_ms,digital_mic_window_ms,analog_mic_window_ms,post_upload_rx_window_sec FROM monitor_config WHERE id=1;", -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return -1; }
    *auto_interval_sec = (uint32_t)sqlite3_column_int64(stmt, 0);
    *poll_interval_sec = (uint16_t)sqlite3_column_int(stmt, 1);
    *vibration_rate_hz = (uint16_t)sqlite3_column_int(stmt, 2);
    *digital_mic_rate_hz = (uint16_t)sqlite3_column_int(stmt, 3);
    *analog_mic_rate_hz = (uint32_t)sqlite3_column_int64(stmt, 4);
    *vibration_window_ms = (uint16_t)sqlite3_column_int(stmt, 5);
    *digital_mic_window_ms = (uint16_t)sqlite3_column_int(stmt, 6);
    *analog_mic_window_ms = (uint16_t)sqlite3_column_int(stmt, 7);
    *post_upload_rx_window_sec = (uint16_t)sqlite3_column_int(stmt, 8);
    sqlite3_finalize(stmt); return 0;
}

static void read_monitor_policy(sqlite3_stmt *stmt, MonitorPolicyRecord_t *record)
{
    memset(record, 0, sizeof(*record));
    record->id = sqlite3_column_int(stmt, 0);
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    snprintf(record->name, sizeof(record->name), "%s", name ? name : "");
    record->is_default = sqlite3_column_int(stmt, 2);
    record->auto_interval_sec = (uint32_t)sqlite3_column_int64(stmt, 3);
    record->poll_interval_sec = (uint16_t)sqlite3_column_int(stmt, 4);
    record->vibration_rate_hz = (uint16_t)sqlite3_column_int(stmt, 5);
    record->digital_mic_rate_hz = (uint16_t)sqlite3_column_int(stmt, 6);
    record->analog_mic_rate_hz = (uint32_t)sqlite3_column_int64(stmt, 7);
    record->vibration_window_ms = (uint16_t)sqlite3_column_int(stmt, 8);
    record->digital_mic_window_ms = (uint16_t)sqlite3_column_int(stmt, 9);
    record->analog_mic_window_ms = (uint16_t)sqlite3_column_int(stmt, 10);
    record->raw_upload_every_cycles = (uint16_t)sqlite3_column_int(stmt, 11);
    record->post_upload_rx_window_sec = (uint16_t)sqlite3_column_int(stmt, 12);
    record->sensor_enable_mask = (uint8_t)sqlite3_column_int(stmt, 13);
}

int get_monitor_policies(MonitorPolicyRecord_t **out_records)
{
    static const char *sql =
        "SELECT id,name,is_default,auto_interval_sec,poll_interval_sec,"
        "vibration_rate_hz,digital_mic_rate_hz,analog_mic_rate_hz,"
        "vibration_window_ms,digital_mic_window_ms,analog_mic_window_ms,"
        "raw_upload_every_cycles,post_upload_rx_window_sec,sensor_enable_mask FROM monitor_policies "
        "ORDER BY is_default DESC,id ASC;";
    sqlite3_stmt *stmt = NULL;
    int count = 0;
    if (!g_db || !out_records) return -1;
    *out_records = NULL;
    if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM monitor_policies;",
                           -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if (count <= 0) return 0;
    MonitorPolicyRecord_t *records = calloc((size_t)count, sizeof(*records));
    if (!records) return -1;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        free(records);
        return -1;
    }
    int index = 0;
    while (index < count && sqlite3_step(stmt) == SQLITE_ROW)
        read_monitor_policy(stmt, &records[index++]);
    sqlite3_finalize(stmt);
    *out_records = records;
    return index;
}

int load_device_monitor_policy(const char *mac, MonitorPolicyRecord_t *policy,
                               uint32_t *raw_upload_generation)
{
    static const char *sql =
        "SELECT p.id,p.name,p.is_default,p.auto_interval_sec,p.poll_interval_sec,"
        "p.vibration_rate_hz,p.digital_mic_rate_hz,p.analog_mic_rate_hz,"
        "p.vibration_window_ms,p.digital_mic_window_ms,p.analog_mic_window_ms,"
        "p.raw_upload_every_cycles,p.post_upload_rx_window_sec,p.sensor_enable_mask,d.raw_upload_generation,d.sampling_admission "
        "FROM devices d JOIN monitor_policies p ON p.id=d.monitor_policy_id "
        "WHERE d.mac=? LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    if (!g_db || !mac || !policy || !raw_upload_generation) return -1;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        return -1;
    }
    read_monitor_policy(stmt, policy);
    *raw_upload_generation = (uint32_t)sqlite3_column_int64(stmt, 14);
    if (policy->id==1 && sqlite3_column_int(stmt,15)!=0) {
        policy->auto_interval_sec=MONITOR_DEFAULT_AUTO_INTERVAL_SEC;
        policy->poll_interval_sec=MONITOR_DEFAULT_POLL_INTERVAL_SEC;
        policy->vibration_rate_hz=MONITOR_DEFAULT_VIBRATION_RATE_HZ;
        policy->digital_mic_rate_hz=MONITOR_DEFAULT_DIGITAL_MIC_RATE_HZ;
        policy->analog_mic_rate_hz=MONITOR_DEFAULT_ANALOG_MIC_RATE_HZ;
        policy->vibration_window_ms=MONITOR_DEFAULT_VIBRATION_WINDOW_MS;
        policy->digital_mic_window_ms=MONITOR_DEFAULT_DIGITAL_MIC_WINDOW_MS;
        policy->analog_mic_window_ms=MONITOR_DEFAULT_ANALOG_MIC_WINDOW_MS;
        policy->raw_upload_every_cycles=MONITOR_DEFAULT_RAW_UPLOAD_EVERY_CYCLES;
        policy->post_upload_rx_window_sec=MONITOR_DEFAULT_POST_UPLOAD_RX_WINDOW_SEC;
        policy->sensor_enable_mask=MONITOR_SENSOR_ALL;
        *raw_upload_generation=1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

static int save_monitor_policy_owned(MonitorPolicyRecord_t *policy,
                        const char *const *node_macs, int node_count,
                        int *out_policy_id, const char *owner_mac)
{
    sqlite3_stmt *stmt = NULL;
    int policy_id;
    int old_raw_cycles = -1;
    int old_sensor_mask = -1;
    if (!g_db || !policy || !policy->name[0] || node_count < 0 ||
        (node_count > 0 && !node_macs)) return -1;
    /* SAVEPOINT permits atomic multi-node/group updates without nested BEGIN. */
    if (sqlite3_exec(g_db, "SAVEPOINT monitor_policy_save;", NULL, NULL, NULL) != SQLITE_OK)
        return -1;

    if (owner_mac) {
        MonitorPolicyRecord_t previous;
        uint32_t generation;
        if (node_count != 1 || strcmp(node_macs[0], owner_mac) != 0 ||
            load_device_monitor_policy(owner_mac, &previous, &generation) != 0) goto rollback;
        /* Preserve hidden settings. Never trust a browser-provided policy id. */
        policy->poll_interval_sec = previous.poll_interval_sec;
        policy->post_upload_rx_window_sec = previous.post_upload_rx_window_sec;
        policy->id = 0;
        if (sqlite3_prepare_v2(g_db, "SELECT id FROM monitor_policies WHERE owner_mac=?;",
                              -1, &stmt, NULL) != SQLITE_OK) goto rollback;
        sqlite3_bind_text(stmt, 1, owner_mac, -1, SQLITE_TRANSIENT);
        int found = sqlite3_step(stmt);
        if (found == SQLITE_ROW) policy->id = sqlite3_column_int(stmt, 0);
        else if (found != SQLITE_DONE) { sqlite3_finalize(stmt); stmt = NULL; goto rollback; }
        sqlite3_finalize(stmt); stmt = NULL;
    }

    if (policy->id > 0)
    {
        if (sqlite3_prepare_v2(g_db,
            "SELECT raw_upload_every_cycles,sensor_enable_mask,owner_mac FROM monitor_policies WHERE id=?;",
            -1, &stmt, NULL) != SQLITE_OK) goto rollback;
        sqlite3_bind_int(stmt, 1, policy->id);
        if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); stmt = NULL; goto rollback; }
        old_raw_cycles = sqlite3_column_int(stmt, 0);
        old_sensor_mask = sqlite3_column_int(stmt, 1);
        const char *stored_owner = (const char *)sqlite3_column_text(stmt, 2);
        if (stored_owner && (!owner_mac || strcmp(stored_owner, owner_mac) != 0)) {
            sqlite3_finalize(stmt); stmt = NULL; goto rollback;
        }
        sqlite3_finalize(stmt); stmt = NULL;
        if (sqlite3_prepare_v2(g_db,
            "UPDATE monitor_policies SET name=?,auto_interval_sec=?,poll_interval_sec=?,"
            "vibration_rate_hz=?,digital_mic_rate_hz=?,analog_mic_rate_hz=?,"
            "vibration_window_ms=?,digital_mic_window_ms=?,analog_mic_window_ms=?,"
            "raw_upload_every_cycles=?,post_upload_rx_window_sec=?,sensor_enable_mask=? WHERE id=?;", -1, &stmt, NULL) != SQLITE_OK)
            goto rollback;
        policy_id = policy->id;
    }
    else
    {
        if (sqlite3_prepare_v2(g_db,
            "INSERT INTO monitor_policies (name,is_default,auto_interval_sec,poll_interval_sec,"
            "vibration_rate_hz,digital_mic_rate_hz,analog_mic_rate_hz,vibration_window_ms,"
            "digital_mic_window_ms,analog_mic_window_ms,raw_upload_every_cycles,"
            "post_upload_rx_window_sec,sensor_enable_mask,owner_mac) VALUES (?,0,?,?,?,?,?,?,?,?,?,?,?,?);", -1, &stmt, NULL) != SQLITE_OK)
            goto rollback;
        policy_id = 0;
    }
    sqlite3_bind_text(stmt, 1, policy->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, policy->auto_interval_sec);
    sqlite3_bind_int(stmt, 3, policy->poll_interval_sec);
    sqlite3_bind_int(stmt, 4, policy->vibration_rate_hz);
    sqlite3_bind_int(stmt, 5, policy->digital_mic_rate_hz);
    sqlite3_bind_int64(stmt, 6, policy->analog_mic_rate_hz);
    sqlite3_bind_int(stmt, 7, policy->vibration_window_ms);
    sqlite3_bind_int(stmt, 8, policy->digital_mic_window_ms);
    sqlite3_bind_int(stmt, 9, policy->analog_mic_window_ms);
    sqlite3_bind_int(stmt, 10, policy->raw_upload_every_cycles);
    sqlite3_bind_int(stmt, 11, policy->post_upload_rx_window_sec);
    sqlite3_bind_int(stmt, 12, policy->sensor_enable_mask);
    if (policy->id > 0) sqlite3_bind_int(stmt, 13, policy->id);
    else if (owner_mac) sqlite3_bind_text(stmt, 13, owner_mac, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 13);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); stmt = NULL; goto rollback; }
    sqlite3_finalize(stmt); stmt = NULL;
    if (policy_id == 0) policy_id = (int)sqlite3_last_insert_rowid(g_db);

    if (old_raw_cycles >= 0 &&
        ((old_raw_cycles != policy->raw_upload_every_cycles) ||
         (((old_sensor_mask ^ policy->sensor_enable_mask) & 0x01) != 0)))
    {
        if (sqlite3_prepare_v2(g_db,
            "UPDATE devices SET raw_upload_generation=raw_upload_generation+1 "
            "WHERE monitor_policy_id=?;", -1, &stmt, NULL) != SQLITE_OK) goto rollback;
        sqlite3_bind_int(stmt, 1, policy_id);
        if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); stmt = NULL; goto rollback; }
        sqlite3_finalize(stmt); stmt = NULL;
    }

    if (sqlite3_exec(g_db,
        "CREATE TEMP TABLE IF NOT EXISTS selected_monitor_nodes(mac TEXT PRIMARY KEY);"
        "DELETE FROM selected_monitor_nodes;", NULL, NULL, NULL) != SQLITE_OK) goto rollback;
    if (sqlite3_prepare_v2(g_db,
        "INSERT OR IGNORE INTO selected_monitor_nodes(mac) VALUES(?);",
        -1, &stmt, NULL) != SQLITE_OK) goto rollback;
    for (int i = 0; i < node_count; ++i)
    {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, node_macs[i], -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); stmt = NULL; goto rollback; }
    }
    sqlite3_finalize(stmt); stmt = NULL;

    if (policy_id != 1)
    {
        if (sqlite3_prepare_v2(g_db,
            "UPDATE devices SET monitor_policy_id=1,raw_upload_generation=raw_upload_generation+1 "
            "WHERE monitor_policy_id=? AND mac NOT IN (SELECT mac FROM selected_monitor_nodes);",
            -1, &stmt, NULL) != SQLITE_OK) goto rollback;
        sqlite3_bind_int(stmt, 1, policy_id);
        if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); stmt = NULL; goto rollback; }
        sqlite3_finalize(stmt); stmt = NULL;
    }
    if (sqlite3_prepare_v2(g_db,
        "UPDATE devices SET raw_upload_generation=raw_upload_generation+1,monitor_policy_id=? "
        "WHERE mac IN (SELECT mac FROM selected_monitor_nodes) AND monitor_policy_id<>?;",
        -1, &stmt, NULL) != SQLITE_OK) goto rollback;
    sqlite3_bind_int(stmt, 1, policy_id);
    sqlite3_bind_int(stmt, 2, policy_id);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); stmt = NULL; goto rollback; }
    sqlite3_finalize(stmt); stmt = NULL;

    if (policy_id == 1 && save_monitor_config(policy->auto_interval_sec,
        policy->poll_interval_sec, policy->vibration_rate_hz,
        policy->digital_mic_rate_hz, policy->analog_mic_rate_hz,
        policy->vibration_window_ms, policy->digital_mic_window_ms,
        policy->analog_mic_window_ms,
        policy->post_upload_rx_window_sec) != 0) goto rollback;

    if (sqlite3_exec(g_db, "RELEASE monitor_policy_save;", NULL, NULL, NULL) != SQLITE_OK) goto rollback;
    policy->id = policy_id;
    if (out_policy_id) *out_policy_id = policy_id;
    return 0;

rollback:
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_exec(g_db, "ROLLBACK TO monitor_policy_save; RELEASE monitor_policy_save;", NULL, NULL, NULL);
    return -1;
}

int save_monitor_policy(MonitorPolicyRecord_t *policy,
                        const char *const *node_macs, int node_count,
                        int *out_policy_id)
{
    return save_monitor_policy_owned(policy, node_macs, node_count, out_policy_id, NULL);
}

int save_device_monitor_policy(const char *mac, MonitorPolicyRecord_t *policy,
                               int *out_policy_id)
{
    const char *nodes[] = {mac};
    if (!mac) return -1;
    return save_monitor_policy_owned(policy, nodes, 1, out_policy_id, mac);
}

int get_node_group_sampling(int id, MonitorPolicyRecord_t *policy)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(g_db, "SELECT auto_interval_sec,vibration_window_ms,digital_mic_window_ms,analog_mic_window_ms,raw_upload_every_cycles,sensor_enable_mask FROM node_group_sampling WHERE group_id=?", -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(s, 1, id);
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        memset(policy, 0, sizeof(*policy));
        snprintf(policy->name, sizeof(policy->name), "%s", "分组应用采样配置");
        policy->auto_interval_sec = (uint32_t)sqlite3_column_int64(s, 0);
        policy->vibration_window_ms = (uint16_t)sqlite3_column_int(s, 1);
        policy->digital_mic_window_ms = (uint16_t)sqlite3_column_int(s, 2);
        policy->analog_mic_window_ms = (uint16_t)sqlite3_column_int(s, 3);
        policy->raw_upload_every_cycles = (uint16_t)sqlite3_column_int(s, 4);
        policy->sensor_enable_mask = (uint8_t)sqlite3_column_int(s, 5);
        policy->poll_interval_sec = 5;
        policy->post_upload_rx_window_sec = 5;
        policy->vibration_rate_hz = 26667;
        policy->digital_mic_rate_hz = 46875;
        policy->analog_mic_rate_hz = 256000;
    }
    sqlite3_finalize(s);
    return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

int apply_node_group_sampling(int id, const MonitorPolicyRecord_t *policy, int *updated)
{
    if (!policy || id <= 0 || policy->auto_interval_sec < MONITOR_MIN_AUTO_INTERVAL_SEC || policy->auto_interval_sec > 2147483 ||
        (policy->vibration_window_ms != 500 && policy->vibration_window_ms != 1000 && policy->vibration_window_ms != 1500) ||
        (policy->digital_mic_window_ms != 500 && policy->digital_mic_window_ms != 1000 && policy->digital_mic_window_ms != 1500 && policy->digital_mic_window_ms != 2000) ||
        (policy->analog_mic_window_ms != 500 && policy->analog_mic_window_ms != 1000 && policy->analog_mic_window_ms != 1500 && policy->analog_mic_window_ms != 2000) ||
        !policy->raw_upload_every_cycles || (policy->sensor_enable_mask & ~7U)) return 2;
    if (updated) *updated = 0;
    sqlite3_mutex *mutex = sqlite3_db_mutex(g_db);
    sqlite3_mutex_enter(mutex);
    sqlite3_stmt *s = NULL;
    char (*macs)[32] = NULL;
    int result = -1, count = 0;
    if (sqlite3_exec(g_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) goto done;
    if (sqlite3_prepare_v2(g_db, "SELECT 1 FROM node_groups WHERE id=?", -1, &s, NULL) != SQLITE_OK) goto rollback;
    sqlite3_bind_int(s, 1, id);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s); s = NULL;
    if (rc != SQLITE_ROW) { result = rc == SQLITE_DONE ? 2 : -1; goto rollback; }
    if (sqlite3_prepare_v2(g_db, "INSERT INTO node_group_sampling VALUES(?,?,?,?,?,?,?) ON CONFLICT(group_id) DO UPDATE SET auto_interval_sec=excluded.auto_interval_sec,vibration_window_ms=excluded.vibration_window_ms,digital_mic_window_ms=excluded.digital_mic_window_ms,analog_mic_window_ms=excluded.analog_mic_window_ms,raw_upload_every_cycles=excluded.raw_upload_every_cycles,sensor_enable_mask=excluded.sensor_enable_mask", -1, &s, NULL) != SQLITE_OK) goto rollback;
    sqlite3_bind_int(s, 1, id);
    sqlite3_bind_int64(s, 2, policy->auto_interval_sec);
    sqlite3_bind_int(s, 3, policy->vibration_window_ms);
    sqlite3_bind_int(s, 4, policy->digital_mic_window_ms);
    sqlite3_bind_int(s, 5, policy->analog_mic_window_ms);
    sqlite3_bind_int(s, 6, policy->raw_upload_every_cycles);
    sqlite3_bind_int(s, 7, policy->sensor_enable_mask);
    if (sqlite3_step(s) != SQLITE_DONE) goto rollback;
    sqlite3_finalize(s); s = NULL;
    count = get_node_group_members(id, &macs);
    if (count < 0) goto rollback;
    for (int i = 0; i < count; ++i) {
        MonitorPolicyRecord_t copy = *policy;
        snprintf(copy.name, sizeof(copy.name), "%s", "分组应用采样配置");
        if (save_device_monitor_policy(macs[i], &copy, NULL) != 0) goto rollback;
    }
    if (sqlite3_exec(g_db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto rollback;
    result = 0;
    if (updated) *updated = count;
    goto done;
rollback:
    sqlite3_finalize(s); s = NULL;
    sqlite3_exec(g_db, "ROLLBACK", NULL, NULL, NULL);
done:
    free(macs);
    sqlite3_mutex_leave(mutex);
    return result;
}

int delete_monitor_policy(int policy_id)
{
    sqlite3_stmt *stmt = NULL;
    if (!g_db || policy_id <= 1) return -1;
    if (sqlite3_exec(g_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) return -1;
    if (sqlite3_prepare_v2(g_db,
        "UPDATE devices SET monitor_policy_id=1,raw_upload_generation=raw_upload_generation+1 "
        "WHERE monitor_policy_id=?;", -1, &stmt, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_int(stmt, 1, policy_id);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); stmt = NULL; goto fail; }
    sqlite3_finalize(stmt); stmt = NULL;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM monitor_policies WHERE id=? AND is_default=0;",
                           -1, &stmt, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_int(stmt, 1, policy_id);
    if (sqlite3_step(stmt) != SQLITE_DONE || sqlite3_changes(g_db) == 0)
    { sqlite3_finalize(stmt); stmt = NULL; goto fail; }
    sqlite3_finalize(stmt);
    return sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
fail:
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
}

int save_zigbee_channel_config(const char *strategy, int target_channel)
{
    sqlite3_stmt *stmt = NULL;
    if (!g_db || !strategy ||
        (strcmp(strategy, "auto") != 0 && strcmp(strategy, "manual") != 0)) return -1;
    if (strcmp(strategy, "manual") == 0 && (target_channel < 11 || target_channel > 26)) return -1;
    if (sqlite3_prepare_v2(g_db,
        "UPDATE zigbee_config SET channel_strategy=?,target_channel=? WHERE id=1;",
        -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, strategy, -1, SQLITE_TRANSIENT);
    if (strcmp(strategy, "manual") == 0) sqlite3_bind_int(stmt, 2, target_channel);
    else sqlite3_bind_null(stmt, 2);
    int ok = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return ok;
}

int load_zigbee_channel_config(char *strategy, size_t strategy_size,
                               int *target_channel)
{
    sqlite3_stmt *stmt = NULL;
    if (!g_db || !strategy || strategy_size == 0 || !target_channel) return -1;
    if (sqlite3_prepare_v2(g_db,
        "SELECT channel_strategy,target_channel FROM zigbee_config WHERE id=1;",
        -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return -1; }
    const char *value = (const char *)sqlite3_column_text(stmt, 0);
    snprintf(strategy, strategy_size, "%s", value ? value : "auto");
    *target_channel = sqlite3_column_type(stmt, 1) == SQLITE_NULL
        ? 0 : sqlite3_column_int(stmt, 1);
    sqlite3_finalize(stmt);
    return 0;
}

int reset_zigbee_channel_config(void)
{
    char *err_msg = NULL;
    if (!g_db) return -1;
    int rc = sqlite3_exec(g_db,
        "UPDATE zigbee_config SET channel_strategy='auto',target_channel=NULL,"
        "actual_pan_id=NULL,actual_channel=NULL,startup_source='unknown' WHERE id=1;",
        0, 0, &err_msg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 重置信道策略失败: %s\n",
                err_msg ? err_msg : "unknown");
        sqlite3_free(err_msg);
        return -1;
    }
    return 0;
}

int save_channel_operation_record(const ChannelOperationRecord_t *record)
{
    sqlite3_stmt *stmt = NULL;
    if (!g_db || !record) return -1;
    const char *sql =
        "UPDATE channel_operation SET state=?,strategy=?,target_channel=?,"
        "current_channel=?,coordinator_status=?,waiting_for_nodes=?,commit_sent=?,"
        "node_count=?,prepared_count=?,message=? WHERE id=1;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, record->state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, record->strategy);
    sqlite3_bind_int(stmt, 3, record->target_channel);
    sqlite3_bind_int(stmt, 4, record->current_channel);
    sqlite3_bind_int(stmt, 5, record->coordinator_status);
    sqlite3_bind_int(stmt, 6, record->waiting_for_nodes);
    sqlite3_bind_int(stmt, 7, record->commit_sent);
    sqlite3_bind_int(stmt, 8, record->node_count);
    sqlite3_bind_int(stmt, 9, record->prepared_count);
    sqlite3_bind_text(stmt, 10, record->message, -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return ok;
}

static void copy_sqlite_error(sqlite3 *db, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    snprintf(out, out_size, "%s", db ? sqlite3_errmsg(db) : "database unavailable");
}

int save_channel_operation_snapshot(const ChannelOperationRecord_t *record,
                                    const ChannelOperationNodeRecord_t *nodes,
                                    int count,
                                    char *error_message,
                                    size_t error_message_size)
{
    sqlite3_stmt *record_stmt = NULL;
    sqlite3_stmt *node_stmt = NULL;
    sqlite3_mutex *db_mutex;
    int rc = SQLITE_ERROR;
    int transaction_started = 0;

    if (error_message && error_message_size > 0) error_message[0] = '\0';
    if (!g_db || !record || count < 0 || (count > 0 && !nodes))
    {
        if (error_message && error_message_size > 0)
            snprintf(error_message, error_message_size, "invalid snapshot arguments");
        return -1;
    }

    db_mutex = sqlite3_db_mutex(g_db);

    /* Some sensor-history writers also use transactions on this shared
     * connection. Retry only until their current transaction has completed,
     * then hold SQLite's recursive connection mutex for this whole snapshot. */
    for (int attempt = 0; attempt < 25; ++attempt)
    {
        sqlite3_mutex_enter(db_mutex);
        rc = sqlite3_exec(g_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
        if (rc == SQLITE_OK)
        {
            transaction_started = 1;
            break;
        }
        copy_sqlite_error(g_db, error_message, error_message_size);
        sqlite3_mutex_leave(db_mutex);
        if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED &&
            strstr(error_message ? error_message : "", "transaction") == NULL)
            return -1;
        sqlite3_sleep(20);
    }

    if (!transaction_started) return -1;

    const char *record_sql =
        "UPDATE channel_operation SET state=?,strategy=?,target_channel=?,"
        "current_channel=?,coordinator_status=?,waiting_for_nodes=?,commit_sent=?,"
        "node_count=?,prepared_count=?,message=? WHERE id=1;";
    rc = sqlite3_prepare_v2(g_db, record_sql, -1, &record_stmt, NULL);
    if (rc != SQLITE_OK) goto rollback;
    sqlite3_bind_text(record_stmt, 1, record->state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(record_stmt, 2, record->strategy);
    sqlite3_bind_int(record_stmt, 3, record->target_channel);
    sqlite3_bind_int(record_stmt, 4, record->current_channel);
    sqlite3_bind_int(record_stmt, 5, record->coordinator_status);
    sqlite3_bind_int(record_stmt, 6, record->waiting_for_nodes);
    sqlite3_bind_int(record_stmt, 7, record->commit_sent);
    sqlite3_bind_int(record_stmt, 8, record->node_count);
    sqlite3_bind_int(record_stmt, 9, record->prepared_count);
    sqlite3_bind_text(record_stmt, 10, record->message, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(record_stmt);
    sqlite3_finalize(record_stmt);
    record_stmt = NULL;
    if (rc != SQLITE_DONE) goto rollback;

    rc = sqlite3_exec(g_db, "DELETE FROM channel_operation_nodes;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto rollback;
    rc = sqlite3_prepare_v2(g_db,
        "INSERT INTO channel_operation_nodes(mac,short_addr,state) VALUES(?,?,?);",
        -1, &node_stmt, NULL);
    if (rc != SQLITE_OK) goto rollback;
    for (int i = 0; i < count; ++i)
    {
        sqlite3_bind_text(node_stmt, 1, nodes[i].mac, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(node_stmt, 2, nodes[i].short_addr);
        sqlite3_bind_int(node_stmt, 3, nodes[i].state);
        rc = sqlite3_step(node_stmt);
        if (rc != SQLITE_DONE) goto rollback;
        sqlite3_reset(node_stmt);
        sqlite3_clear_bindings(node_stmt);
    }
    sqlite3_finalize(node_stmt);
    node_stmt = NULL;

    rc = sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto rollback;
    sqlite3_mutex_leave(db_mutex);
    return 0;

rollback:
    copy_sqlite_error(g_db, error_message, error_message_size);
    if (record_stmt) sqlite3_finalize(record_stmt);
    if (node_stmt) sqlite3_finalize(node_stmt);
    sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
    sqlite3_mutex_leave(db_mutex);
    return -1;
}

int load_channel_operation_record(ChannelOperationRecord_t *record)
{
    sqlite3_stmt *stmt = NULL;
    if (!g_db || !record) return -1;
    memset(record, 0, sizeof(*record));
    const char *sql =
        "SELECT state,strategy,target_channel,current_channel,coordinator_status,"
        "waiting_for_nodes,commit_sent,node_count,prepared_count,message "
        "FROM channel_operation WHERE id=1;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return -1; }
    const char *state = (const char *)sqlite3_column_text(stmt, 0);
    const char *message = (const char *)sqlite3_column_text(stmt, 9);
    snprintf(record->state, sizeof(record->state), "%s", state ? state : "idle");
    record->strategy = sqlite3_column_int(stmt, 1);
    record->target_channel = sqlite3_column_int(stmt, 2);
    record->current_channel = sqlite3_column_int(stmt, 3);
    record->coordinator_status = sqlite3_column_int(stmt, 4);
    record->waiting_for_nodes = sqlite3_column_int(stmt, 5);
    record->commit_sent = sqlite3_column_int(stmt, 6);
    record->node_count = sqlite3_column_int(stmt, 7);
    record->prepared_count = sqlite3_column_int(stmt, 8);
    snprintf(record->message, sizeof(record->message), "%s", message ? message : "");
    sqlite3_finalize(stmt);
    return 0;
}

int replace_channel_operation_nodes(const ChannelOperationNodeRecord_t *nodes,
                                    int count)
{
    if (!g_db || count < 0 || (count > 0 && !nodes)) return -1;
    char *err_msg = NULL;
    if (sqlite3_exec(g_db, "BEGIN;DELETE FROM channel_operation_nodes;",
                    0, 0, &err_msg) != SQLITE_OK)
    {
        sqlite3_free(err_msg);
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    int ok = sqlite3_prepare_v2(g_db,
        "INSERT INTO channel_operation_nodes(mac,short_addr,state) VALUES(?,?,?);",
        -1, &stmt, NULL) == SQLITE_OK ? 0 : -1;
    for (int i = 0; ok == 0 && i < count; ++i)
    {
        sqlite3_bind_text(stmt, 1, nodes[i].mac, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, nodes[i].short_addr);
        sqlite3_bind_int(stmt, 3, nodes[i].state);
        if (sqlite3_step(stmt) != SQLITE_DONE) ok = -1;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    if (stmt) sqlite3_finalize(stmt);
    if (sqlite3_exec(g_db, ok == 0 ? "COMMIT;" : "ROLLBACK;",
                    0, 0, &err_msg) != SQLITE_OK)
    {
        sqlite3_free(err_msg);
        return -1;
    }
    return ok;
}

int load_channel_operation_nodes(ChannelOperationNodeRecord_t **nodes,
                                 int *count)
{
    sqlite3_stmt *stmt = NULL;
    if (!g_db || !nodes || !count) return -1;
    *nodes = NULL;
    *count = 0;
    if (sqlite3_prepare_v2(g_db,
        "SELECT mac,short_addr,state FROM channel_operation_nodes ORDER BY rowid;",
        -1, &stmt, NULL) != SQLITE_OK) return -1;
    int capacity = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        if (*count == capacity)
        {
            int next_capacity = capacity == 0 ? 8 : capacity * 2;
            ChannelOperationNodeRecord_t *next = realloc(
                *nodes, (size_t)next_capacity * sizeof(**nodes));
            if (!next)
            {
                free(*nodes); *nodes = NULL; *count = 0;
                sqlite3_finalize(stmt); return -1;
            }
            *nodes = next;
            capacity = next_capacity;
        }
        ChannelOperationNodeRecord_t *node = &(*nodes)[*count];
        const char *mac = (const char *)sqlite3_column_text(stmt, 0);
        snprintf(node->mac, sizeof(node->mac), "%s", mac ? mac : "");
        node->short_addr = (uint16_t)sqlite3_column_int(stmt, 1);
        node->state = (uint8_t)sqlite3_column_int(stmt, 2);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    return 0;
}

int update_channel_operation_node(const char *mac, uint16_t short_addr,
                                  uint8_t state)
{
    sqlite3_stmt *stmt = NULL;
    if (!g_db || !mac) return -1;
    if (sqlite3_prepare_v2(g_db,
        "UPDATE channel_operation_nodes SET short_addr=?,state=? WHERE mac=?;",
        -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, short_addr);
    sqlite3_bind_int(stmt, 2, state);
    sqlite3_bind_text(stmt, 3, mac, -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return ok;
}

int delete_channel_operation_node(const char *mac)
{
    sqlite3_stmt *stmt = NULL;
    if (!g_db || !mac) return -1;
    if (sqlite3_prepare_v2(g_db,
        "DELETE FROM channel_operation_nodes WHERE mac=?;",
        -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return ok;
}

int save_zigbee_network_state(uint16_t pan_id, uint8_t actual_channel,
                              const char *startup_source)
{
    sqlite3_stmt *stmt = NULL;
    if (!g_db || !startup_source || actual_channel < 11U || actual_channel > 26U)
        return -1;
    if (sqlite3_prepare_v2(g_db,
        "UPDATE zigbee_config SET actual_pan_id=?,actual_channel=?,startup_source=? WHERE id=1;",
        -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, pan_id);
    sqlite3_bind_int(stmt, 2, actual_channel);
    sqlite3_bind_text(stmt, 3, startup_source, -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return ok;
}

int clear_zigbee_network_state(void)
{
    char *err_msg = NULL;
    if (!g_db) return -1;
    int rc = sqlite3_exec(g_db,
        "UPDATE zigbee_config SET actual_pan_id=NULL,actual_channel=NULL,startup_source='unknown' WHERE id=1;",
        0, 0, &err_msg);
    if (rc != SQLITE_OK)
    {
        sqlite3_free(err_msg);
        return -1;
    }
    return 0;
}

int load_zigbee_network_state(uint16_t *pan_id, uint8_t *actual_channel,
                              char *startup_source, size_t startup_source_size)
{
    sqlite3_stmt *stmt = NULL;
    if (!g_db || !pan_id || !actual_channel || !startup_source || startup_source_size == 0U)
        return -1;
    if (sqlite3_prepare_v2(g_db,
        "SELECT actual_pan_id,actual_channel,startup_source FROM zigbee_config WHERE id=1;",
        -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(stmt) != SQLITE_ROW ||
        sqlite3_column_type(stmt, 0) == SQLITE_NULL ||
        sqlite3_column_type(stmt, 1) == SQLITE_NULL)
    {
        sqlite3_finalize(stmt);
        return -1;
    }
    *pan_id = (uint16_t)sqlite3_column_int(stmt, 0);
    *actual_channel = (uint8_t)sqlite3_column_int(stmt, 1);
    const char *source = (const char *)sqlite3_column_text(stmt, 2);
    snprintf(startup_source, startup_source_size, "%s", source ? source : "unknown");
    sqlite3_finalize(stmt);
    return 0;
}

int save_vibration_batch(const char *mac, uint32_t batch_id, uint32_t sample_rate_hz,
                         uint32_t sample_count, const void *samples, size_t sample_bytes,
                         const int32_t features[3][5])
{
    sqlite3_stmt *stmt = NULL;
    int existing = 0;
    const char *find_sql =
        "SELECT 1 FROM vibration_batches WHERE mac=? AND batch_id=? LIMIT 1;";
    if (!g_db || !mac || !features || (sample_bytes > 0U && !samples) ||
        sqlite3_prepare_v2(g_db, find_sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)batch_id);
    existing = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);

    if (!existing)
    {
        const char *insert_sql = "INSERT INTO vibration_batches(mac,batch_id,sample_rate_hz,sample_count,samples,features,timestamp) VALUES(?,?,?,?,?,?,CAST(strftime('%s','now') AS INTEGER));";
        if (sqlite3_prepare_v2(g_db, insert_sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
        sqlite3_bind_text(stmt,1,mac,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt,2,(sqlite3_int64)batch_id);
        sqlite3_bind_int64(stmt,3,sample_rate_hz); sqlite3_bind_int64(stmt,4,sample_count);
        if (sample_bytes > 0U) sqlite3_bind_blob(stmt,5,samples,(int)sample_bytes,SQLITE_TRANSIENT);
        else sqlite3_bind_zeroblob(stmt,5,0);
        sqlite3_bind_blob(stmt,6,features,(int)(sizeof(int32_t)*15U),SQLITE_TRANSIENT);
        int rc=sqlite3_step(stmt); sqlite3_finalize(stmt); return rc==SQLITE_DONE?0:-1;
    }

    /* A replayed deferred V2 descriptor must not overwrite a final 0x8C
       summary already committed for this MAC+batch. */
    int32_t retained_features[3][5];
    if (v2_features_pending(features))
    {
        if (sqlite3_prepare_v2(g_db,
            "SELECT features FROM vibration_batches WHERE mac=? AND batch_id=?;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
        sqlite3_bind_text(stmt,1,mac,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt,2,(sqlite3_int64)batch_id);
        if (sqlite3_step(stmt)==SQLITE_ROW &&
            sqlite3_column_bytes(stmt,0)==(int)sizeof(retained_features))
        {
            memcpy(retained_features,sqlite3_column_blob(stmt,0),sizeof(retained_features));
            if (!v2_features_pending(retained_features)) features=retained_features;
        }
        sqlite3_finalize(stmt);
    }

    const char *update_sql =
        "UPDATE vibration_batches SET sample_rate_hz=?,sample_count=?,"
        "samples=CASE WHEN ?>0 THEN ? ELSE samples END,features=?,"
        "timestamp=CAST(strftime('%s','now') AS INTEGER) WHERE mac=? AND batch_id=?;";
    if (sqlite3_prepare_v2(g_db, update_sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt,1,sample_rate_hz); sqlite3_bind_int64(stmt,2,sample_count);
    sqlite3_bind_int64(stmt,3,(sqlite3_int64)sample_bytes);
    if (sample_bytes > 0U) sqlite3_bind_blob(stmt,4,samples,(int)sample_bytes,SQLITE_TRANSIENT);
    else sqlite3_bind_zeroblob(stmt,4,0);
    sqlite3_bind_blob(stmt,5,features,(int)(sizeof(int32_t)*15U),SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,6,mac,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt,7,(sqlite3_int64)batch_id);
    int rc=sqlite3_step(stmt); sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int vibration_summary_matches(const char *mac,uint32_t batch,uint32_t rate,
                               uint32_t count,const int32_t features[3][5])
{
    sqlite3_stmt *stmt=NULL;
    const char *sql="SELECT sample_rate_hz,sample_count,features FROM vibration_batches WHERE mac=? AND batch_id=?;";
    if(!g_db || !mac || !features || sqlite3_prepare_v2(g_db,sql,-1,&stmt,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(stmt,1,mac,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt,2,(sqlite3_int64)batch);
    int rc=sqlite3_step(stmt),result=0;
    if(rc==SQLITE_ROW)result=sqlite3_column_int64(stmt,0)==rate && sqlite3_column_int64(stmt,1)==count &&
        sqlite3_column_bytes(stmt,2)==(int)(sizeof(int32_t)*15U) &&
        !memcmp(sqlite3_column_blob(stmt,2),features,sizeof(int32_t)*15U);
    else if(rc!=SQLITE_DONE)result=-1;
    sqlite3_finalize(stmt);return result;
}

int remember_upload_batch(const char *mac,const uint8_t descriptor[77],unsigned raw_required)
{
    sqlite3_stmt *s=NULL;
    const char *sql="INSERT INTO upload_batch_receipts(mac,batch_id,identity,raw_required) VALUES(?1,?2,?3,?4) "
        "ON CONFLICT(mac,batch_id) DO UPDATE SET identity=excluded.identity "
        "WHERE identity=excluded.identity AND raw_required=excluded.raw_required;";
    if(!g_db || !mac || raw_required>1U || !v2_valid_batch(descriptor,V2_BATCH_LEN) ||
       sqlite3_prepare_v2(g_db,sql,-1,&s,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(s,2,v2_u32(descriptor+1));
    sqlite3_bind_blob(s,3,descriptor,17,SQLITE_TRANSIENT);
    sqlite3_bind_int(s,4,(int)raw_required);
    int rc=sqlite3_step(s),changed=sqlite3_changes(g_db);
    sqlite3_finalize(s);
    return rc==SQLITE_DONE && changed==1?0:-1;
}

int remember_upload_manifest(const char *mac,uint32_t batch,const uint8_t manifest[24])
{
    sqlite3_stmt *s=NULL;
    const char *sql="SELECT manifest FROM upload_batch_receipts WHERE mac=?1 AND batch_id=?2;";
    if(!g_db || !mac || !manifest || sqlite3_prepare_v2(g_db,sql,-1,&s,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,2,batch);
    int rc=sqlite3_step(s);
    if(rc==SQLITE_DONE){sqlite3_finalize(s);return 0;} /* old node, no completion extension */
    if(rc!=SQLITE_ROW){sqlite3_finalize(s);return -1;}
    int bytes=sqlite3_column_bytes(s,0);
    int same=bytes==24 && !memcmp(sqlite3_column_blob(s,0),manifest,24);
    sqlite3_finalize(s);
    if(bytes)return same?0:-1;
    if(sqlite3_prepare_v2(g_db,"UPDATE upload_batch_receipts SET manifest=?3 WHERE mac=?1 AND batch_id=?2 AND manifest IS NULL;",-1,&s,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,2,batch);
    sqlite3_bind_blob(s,3,manifest,24,SQLITE_TRANSIENT);
    rc=sqlite3_step(s);sqlite3_finalize(s);return rc==SQLITE_DONE?0:-1;
}

int next_gateway_exchange_sequence(const char *mac,uint32_t token,uint16_t *seq)
{
    if (!g_db || !mac || !token || !seq) return 0;
    static const char advance[] =
        "INSERT INTO gateway_exchange_sequence(mac,token,seq,touched) VALUES(?1,?2,1,strftime('%s','now')) "
        "ON CONFLICT(mac,token) DO UPDATE SET seq=seq%65535+1,touched=excluded.touched;";
    static const char read[] =
        "SELECT seq FROM gateway_exchange_sequence WHERE mac=?1 AND token=?2;";
    sqlite3_mutex *mutex=sqlite3_db_mutex(g_db);
    sqlite3_mutex_enter(mutex);
    sqlite3_stmt *stmt=NULL;
    int result=0;
    if (sqlite3_prepare_v2(g_db,advance,-1,&stmt,NULL)==SQLITE_OK) {
        sqlite3_bind_text(stmt,1,mac,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt,2,token);
        result=sqlite3_step(stmt)==SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    stmt=NULL;
    if (result && sqlite3_prepare_v2(g_db,read,-1,&stmt,NULL)==SQLITE_OK) {
        sqlite3_bind_text(stmt,1,mac,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt,2,token);
        result=sqlite3_step(stmt)==SQLITE_ROW;
        if (result) *seq=(uint16_t)sqlite3_column_int(stmt,0);
    } else result=0;
    sqlite3_finalize(stmt);
    stmt=NULL;
    /* Keep the most recently used tokens for each MAC across backend restarts. */
    if (result && *seq==1U && sqlite3_prepare_v2(g_db,
        "DELETE FROM gateway_exchange_sequence WHERE mac=?1 AND token<>?2 "
        "AND touched<strftime('%s','now','-1 day');",-1,&stmt,NULL)==SQLITE_OK) {
        sqlite3_bind_text(stmt,1,mac,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt,2,token);
        (void)sqlite3_step(stmt);
    }
    if (result && *seq==1U) sqlite3_finalize(stmt);
    sqlite3_mutex_leave(mutex);
    return result;
}

int gateway_exchange_command_record(const char *mac,uint32_t token,uint8_t *frame,unsigned len,int save)
{
    if (!g_db || !mac || !token || !frame || !len) return -1;
    const char *sql=save ?
        "INSERT INTO gateway_exchange_command(mac,token,frame) VALUES(?1,?2,?3) "
        "ON CONFLICT(mac,token) DO UPDATE SET frame=excluded.frame;" :
        "SELECT frame FROM gateway_exchange_command WHERE mac=?1 AND token=?2;";
    sqlite3_stmt *stmt=NULL;
    if (sqlite3_prepare_v2(g_db,sql,-1,&stmt,NULL)!=SQLITE_OK) return -1;
    sqlite3_bind_text(stmt,1,mac,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt,2,token);
    if (save) sqlite3_bind_blob(stmt,3,frame,(int)len,SQLITE_TRANSIENT);
    int rc=sqlite3_step(stmt),result=-1;
    if (save && rc==SQLITE_DONE) result=1;
    else if (!save && rc==SQLITE_DONE) result=0;
    else if (!save && rc==SQLITE_ROW && sqlite3_column_bytes(stmt,0)==(int)len) {
        memcpy(frame,sqlite3_column_blob(stmt,0),len);
        result=1;
    }
    sqlite3_finalize(stmt);
    return result;
}

int get_completed_upload_batch(const char *mac,uint32_t batch,UploadBatchReceipt_t *receipt)
{
    sqlite3_stmt *s=NULL;
    const char *sql="SELECT r.identity,r.raw_required,r.manifest,v.sample_rate_hz,v.sample_count,v.samples,v.features "
        "FROM upload_batch_receipts r JOIN vibration_batches v ON v.mac=r.mac AND v.batch_id=r.batch_id "
        "WHERE r.mac=?1 AND r.batch_id=?2 LIMIT 1;";
    if(!g_db || !mac || !receipt || sqlite3_prepare_v2(g_db,sql,-1,&s,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,2,batch);
    int rc=sqlite3_step(s),result=0;
    if(rc==SQLITE_ROW && sqlite3_column_bytes(s,0)==17 && sqlite3_column_bytes(s,2)==24 &&
       sqlite3_column_bytes(s,6)==60) {
        UploadBatchReceipt_t found;
        memcpy(found.identity,sqlite3_column_blob(s,0),17);
        memcpy(found.manifest,sqlite3_column_blob(s,2),24);
        found.raw_required=(unsigned)sqlite3_column_int(s,1);
        const uint8_t *id=found.identity;
        int32_t features[3][5];memcpy(features,sqlite3_column_blob(s,6),sizeof(features));
        int features_valid=1;
        for(unsigned i=0;i<15;++i)if(features[i/5][i%5]==INT32_MIN)features_valid=0;
        int bytes=sqlite3_column_bytes(s,5);
        const uint8_t *raw=sqlite3_column_blob(s,5);
        if(id[0]==V2_VERSION && v2_u32(id+1)==batch && v2_u16(id+11)==V2_POINTS &&
           v2_u16(id+5)==26667U && v2_u32(id+7)>0 && v2_u32(id+7)<=V2_MAX_SAMPLES &&
           found.raw_required<=1 && found.manifest[0]==V2_VERSION && v2_u32(found.manifest+1)==batch &&
           sqlite3_column_int64(s,3)==v2_u16(id+5) && sqlite3_column_int64(s,4)==v2_u32(id+7) &&
           features_valid && (!found.raw_required ||
             (bytes==(int)(v2_u32(id+7)*6U) && raw && v2_crc(raw,(size_t)bytes)==v2_u32(id+13)))) {
            *receipt=found;result=1;
        }
    } else if(rc!=SQLITE_ROW && rc!=SQLITE_DONE)result=-1;
    sqlite3_finalize(s);return result;
}

int vibration_batch_matches(const char *mac, const uint8_t descriptor[77])
{
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT sample_rate_hz,sample_count,samples,features FROM vibration_batches WHERE mac=? AND batch_id=? LIMIT 1;";
    if (!g_db || !mac || !v2_valid_batch(descriptor,V2_BATCH_LEN) ||
        sqlite3_prepare_v2(g_db,sql,-1,&stmt,NULL)!=SQLITE_OK) return -1;
    sqlite3_bind_text(stmt,1,mac,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt,2,(sqlite3_int64)v2_u32(descriptor+1));
    int step=sqlite3_step(stmt),result=0;
    if (step==SQLITE_ROW)
    {
        int bytes=sqlite3_column_bytes(stmt,2);
        const uint8_t *raw=(const uint8_t *)sqlite3_column_blob(stmt,2);
        int32_t features[3][5];
        for (unsigned i=0;i<15;++i) features[i/5][i%5]=(int32_t)v2_u32(descriptor+17+i*4);
        if (sqlite3_column_int64(stmt,0)!=v2_u16(descriptor+5) ||
            sqlite3_column_int64(stmt,1)!=v2_u32(descriptor+7) ||
            (!v2_features_pending(features) &&
             (sqlite3_column_bytes(stmt,3)!=(int)sizeof(features) ||
              memcmp(sqlite3_column_blob(stmt,3),features,sizeof(features)))))
            result=-1;
        else if (bytes>0)
            result=(bytes==(int)(v2_u32(descriptor+7)*6U) && raw &&
                    v2_crc(raw,(size_t)bytes)==v2_u32(descriptor+13))?1:-1;
    }
    else if (step!=SQLITE_DONE) result=-1;
    sqlite3_finalize(stmt);
    return result;
}

int get_latest_device_readings(const char *mac, LatestDeviceReadings_t *readings)
{
    static const char *queries[] = {
        "SELECT temperature,timestamp,batch_id FROM collection_values WHERE mac=? AND temperature IS NOT NULL ORDER BY timestamp DESC,rowid DESC LIMIT 1;",
        "SELECT mag_x,mag_y,mag_z,timestamp,batch_id FROM collection_values WHERE mac=? AND mag_x IS NOT NULL AND mag_y IS NOT NULL AND mag_z IS NOT NULL ORDER BY timestamp DESC,rowid DESC LIMIT 1;",
        "SELECT digital_db,timestamp,batch_id FROM collection_values WHERE mac=? AND digital_db IS NOT NULL ORDER BY timestamp DESC,rowid DESC LIMIT 1;",
        "SELECT analog_db,timestamp,batch_id FROM collection_values WHERE mac=? AND analog_db IS NOT NULL ORDER BY timestamp DESC,rowid DESC LIMIT 1;"
    };
    static const unsigned offsets[] = {0U, 1U, 4U, 5U};
    sqlite3_stmt *stmt = NULL;
    if (!g_db || !mac || !readings) return 0;
    memset(readings, 0, sizeof(*readings));
    for (unsigned metric = 0; metric < 4; metric++)
    {
        if (sqlite3_prepare_v2(g_db, queries[metric], -1, &stmt, NULL) != SQLITE_OK) continue;
        sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            unsigned count = metric == 1U ? 3U : 1U;
            for (unsigned axis = 0; axis < count; axis++)
            {
                readings->values[offsets[metric] + axis] = (float)sqlite3_column_double(stmt, (int)axis);
                readings->present_mask |= 1U << (offsets[metric] + axis);
            }
            long long timestamp = sqlite3_column_int64(stmt, (int)count);
            if (timestamp > readings->timestamp) {
                readings->timestamp = timestamp;
                readings->batch_id = (uint32_t)sqlite3_column_int64(stmt, (int)count + 1);
            }
        }
        sqlite3_finalize(stmt);
        stmt = NULL;
    }
    /* The newest row may only be the descriptor of a queued upload. Never
     * let its pending sentinel hide the preceding durable feature values. */
    if (sqlite3_prepare_v2(g_db,
        "SELECT batch_id,features,timestamp FROM vibration_batches WHERE mac=? AND length(features)=60 ORDER BY id DESC;",
        -1, &stmt, NULL) == SQLITE_OK)
    {
        sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            memcpy(readings->features, sqlite3_column_blob(stmt, 1), sizeof(readings->features));
            if (v2_features_pending(readings->features)) continue;
            readings->features_ready = 1;
            readings->features_batch_id = (uint32_t)sqlite3_column_int64(stmt, 0);
            readings->features_timestamp = sqlite3_column_int64(stmt, 2);
            if (readings->features_timestamp >= readings->timestamp) {
                readings->timestamp = readings->features_timestamp;
                readings->batch_id = readings->features_batch_id;
            }
            break;
        }
        sqlite3_finalize(stmt);
    }
    return readings->present_mask != 0U || readings->features_ready;
}

int get_vibration_batch(const char *mac, uint32_t requested_batch, int completed_only,
                               uint32_t *batch_id, uint32_t *sample_rate_hz,
                               uint32_t *sample_count, void **samples, size_t *sample_bytes,
                               int32_t features[3][5], long long *timestamp)
{
    sqlite3_stmt *stmt=NULL;
    const char *sql="SELECT batch_id,sample_rate_hz,sample_count,samples,features,timestamp FROM vibration_batches "
        "WHERE mac=? AND (?=0 OR batch_id=?) AND (?=0 OR (sample_count>0 "
        "AND length(samples)=sample_count*6 AND length(features)=60 AND features!=?)) ORDER BY id DESC LIMIT 1;";
    if(!g_db||!mac||!samples||!sample_bytes||sqlite3_prepare_v2(g_db,sql,-1,&stmt,NULL)!=SQLITE_OK)return 0;
    sqlite3_bind_text(stmt,1,mac,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt,2,requested_batch);
    sqlite3_bind_int64(stmt,3,requested_batch);
    sqlite3_bind_int(stmt,4,completed_only);
    int32_t pending[15];
    for (unsigned i=0;i<15;++i) pending[i]=INT32_MIN;
    sqlite3_bind_blob(stmt,5,pending,sizeof(pending),SQLITE_TRANSIENT);
    if(sqlite3_step(stmt)!=SQLITE_ROW){sqlite3_finalize(stmt);return 0;}
    int bytes=sqlite3_column_bytes(stmt,3); const void *blob=sqlite3_column_blob(stmt,3);
    *samples=NULL;
    if(bytes>0){*samples=malloc((size_t)bytes); if(!*samples){sqlite3_finalize(stmt);return 0;} memcpy(*samples,blob,(size_t)bytes);}
    *sample_bytes=(size_t)bytes;
    *batch_id=(uint32_t)sqlite3_column_int64(stmt,0); *sample_rate_hz=(uint32_t)sqlite3_column_int64(stmt,1);
    *sample_count=(uint32_t)sqlite3_column_int64(stmt,2); *timestamp=sqlite3_column_int64(stmt,5);
    if(sqlite3_column_bytes(stmt,4)==(int)(sizeof(int32_t)*15U)) memcpy(features,sqlite3_column_blob(stmt,4),sizeof(int32_t)*15U); else memset(features,0,sizeof(int32_t)*15U);
    sqlite3_finalize(stmt); return 1;
}


int get_latest_vibration_batch(const char *mac, uint32_t *batch_id, uint32_t *sample_rate_hz,
                               uint32_t *sample_count, void **samples, size_t *sample_bytes,
                               int32_t features[3][5], long long *timestamp)
{
    return get_vibration_batch(mac,0,0,batch_id,sample_rate_hz,sample_count,
                               samples,sample_bytes,features,timestamp);
}

// 系统从1970校正到真实时间后，避免在线节点因旧last_active被立即判定掉线。
int refresh_online_device_activity(void)
{
    if (!g_db) return -1;

    char *err_msg = NULL;
    int rc = sqlite3_exec(
        g_db,
        "UPDATE devices SET last_active = CAST(strftime('%s','now') AS INTEGER) "
        "WHERE lower(status) = 'online';",
        0, 0, &err_msg);

    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 授时后刷新节点活动时间失败: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    return 0;
}


// 清理超过指定保存时间的温度、磁场和麦克风历史数据
int cleanup_sensor_history(int retention_seconds)
{
    if (!g_db) return -1;

    sqlite3_stmt *stmt = NULL;
    int total_deleted = 0;

    // 清理温度历史
    const char *sql_delete_temperature =
        "DELETE FROM temperature_data "
        "WHERE timestamp < CAST(strftime('%s','now') AS INTEGER) - ?;";

    if (sqlite3_prepare_v2(g_db, sql_delete_temperature, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 准备清理温度历史失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, retention_seconds);

    if (sqlite3_step(stmt) != SQLITE_DONE)
    {
        fprintf(stderr, "❌ [数据库] 清理温度历史失败: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -1;
    }

    total_deleted += sqlite3_changes(g_db);
    sqlite3_finalize(stmt);


    // 清理磁场历史
    const char *sql_delete_magnetic =
        "DELETE FROM magnetic_data "
        "WHERE timestamp < CAST(strftime('%s','now') AS INTEGER) - ?;";

    if (sqlite3_prepare_v2(g_db, sql_delete_magnetic, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 准备清理磁场历史失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, retention_seconds);

    if (sqlite3_step(stmt) != SQLITE_DONE)
    {
        fprintf(stderr, "❌ [数据库] 清理磁场历史失败: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -1;
    }

    total_deleted += sqlite3_changes(g_db);
    sqlite3_finalize(stmt);


    // 清理麦克风历史
    const char *sql_delete_sound =
        "DELETE FROM sound_data "
        "WHERE timestamp < CAST(strftime('%s','now') AS INTEGER) - ?;";

    if (sqlite3_prepare_v2(g_db, sql_delete_sound, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 准备清理麦克风历史失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, retention_seconds);

    if (sqlite3_step(stmt) != SQLITE_DONE)
    {
        fprintf(stderr, "❌ [数据库] 清理麦克风历史失败: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -1;
    }

    total_deleted += sqlite3_changes(g_db);
    sqlite3_finalize(stmt);

    return total_deleted;
}


void close_database(void) 
{
    if (g_db) 
    {
        sqlite3_close(g_db);
        g_db = NULL;
        printf("✅ [数据库] 连接已安全关闭。\n");
    }
}


// 查询指定设备最近一段时间内的温度历史数据
int get_temperature_history(const char *mac,int history_seconds,TemperatureRecord_t **out_records)
{
    if (!g_db || !mac || !out_records || history_seconds <= 0)
    {
        return 0;
    }

    *out_records = NULL;

    sqlite3_stmt *stmt = NULL;

    /*
     * 先查询符合条件的数据条数，
     * 后面根据条数一次性申请内存。
     */
    const char *sql_count =
        "SELECT COUNT(*) "
        "FROM temperature_data "
        "WHERE mac = ? "
        "AND timestamp >= CAST(strftime('%s','now') AS INTEGER) - ?;";

    if (sqlite3_prepare_v2(g_db, sql_count, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 准备统计温度历史失败: %s\n", sqlite3_errmsg(g_db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, history_seconds);

    int count = 0;

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        count = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);

    if (count <= 0)
    {
        return 0;
    }

    /*
     * 根据实际记录数量申请内存。
     * 调用者使用完成后负责 free()。
     */
    *out_records = (TemperatureRecord_t *)malloc(
        sizeof(TemperatureRecord_t) * count
    );

    if (*out_records == NULL)
    {
        fprintf(stderr, "❌ [数据库] 温度历史内存申请失败！\n");
        return 0;
    }

    /*
     * 按时间从旧到新查询。
     *
     * 这里会命中之前建立的联合索引：
     * (mac, timestamp)
     */
    const char *sql_query =
        "SELECT temperature, timestamp "
        "FROM temperature_data "
        "WHERE mac = ? "
        "AND timestamp >= CAST(strftime('%s','now') AS INTEGER) - ? "
        "ORDER BY timestamp ASC;";

    if (sqlite3_prepare_v2(g_db, sql_query, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 准备查询温度历史失败: %s\n", sqlite3_errmsg(g_db));

        free(*out_records);
        *out_records = NULL;

        return 0;
    }

    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, history_seconds);

    int idx = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && idx < count)
    {
        (*out_records)[idx].temperature =
            (float)sqlite3_column_double(stmt, 0);

        (*out_records)[idx].timestamp =
            sqlite3_column_int64(stmt, 1);

        idx++;
    }

    sqlite3_finalize(stmt);

    return idx;
}


// 查询指定设备最近一段时间内的数字和超声麦克风历史数据
int get_sound_history(const char *mac,int history_seconds,SoundRecord_t **out_records)
{
    if (!g_db || !mac || !out_records || history_seconds <= 0)
    {
        return 0;
    }

    *out_records = NULL;
    sqlite3_stmt *stmt = NULL;

    const char *sql_count =
        "SELECT COUNT(*) FROM sound_data "
        "WHERE mac = ? "
        "AND timestamp >= CAST(strftime('%s','now') AS INTEGER) - ?;";

    if (sqlite3_prepare_v2(g_db, sql_count, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 准备统计麦克风历史失败: %s\n", sqlite3_errmsg(g_db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, history_seconds);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    if (count <= 0)
    {
        return 0;
    }

    *out_records = (SoundRecord_t *)malloc(sizeof(SoundRecord_t) * count);
    if (*out_records == NULL)
    {
        fprintf(stderr, "❌ [数据库] 麦克风历史内存申请失败！\n");
        return 0;
    }

    const char *sql_query =
        "SELECT db_spl, source, timestamp FROM sound_data "
        "WHERE mac = ? "
        "AND timestamp >= CAST(strftime('%s','now') AS INTEGER) - ? "
        "ORDER BY timestamp ASC, source ASC;";

    if (sqlite3_prepare_v2(g_db, sql_query, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "❌ [数据库] 准备查询麦克风历史失败: %s\n", sqlite3_errmsg(g_db));
        free(*out_records);
        *out_records = NULL;
        return 0;
    }

    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, history_seconds);

    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < count)
    {
        (*out_records)[idx].db_spl = (float)sqlite3_column_double(stmt, 0);
        (*out_records)[idx].is_digital = sqlite3_column_int(stmt, 1) ? 1 : 0;
        (*out_records)[idx].timestamp = sqlite3_column_int64(stmt, 2);
        idx++;
    }

    sqlite3_finalize(stmt);
    return idx;
}
