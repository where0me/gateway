#include <assert.h>
#include <stdio.h>
#define GATEWAY_DATABASE_PATH "out/vibration-tests/node-sampling-test.db"
#include "../gateway-core/src/db_manager.c"

static int owner_count(void)
{
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM monitor_policies WHERE owner_mac IS NOT NULL;", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

int main(void)
{
    /* Only this isolated test file is recreated; no production database access. */
    remove(GATEWAY_DATABASE_PATH);
    const char *a = "01:02:03:04:05:06:07:08";
    const char *b = "11:12:13:14:15:16:17:18";
    assert(init_database() == 0);
    assert(insert_new_device(a, "A") == 0 && insert_new_device(b, "B") == 0);
    /* Simulate the previous schema and exercise automatic migration. */
    assert(sqlite3_exec(g_db, "DROP INDEX monitor_policy_owner; ALTER TABLE monitor_policies DROP COLUMN owner_mac;", NULL, NULL, NULL) == SQLITE_OK);
    close_database();
    assert(init_database() == 0);
    MonitorPolicyRecord_t original, policy, other, loaded;
    uint32_t initial, generation, other_generation;
    assert(load_device_monitor_policy(a, &original, &initial) == 0);
    policy = original;
    policy.auto_interval_sec = 600;
    policy.vibration_window_ms = 1000;
    policy.poll_interval_sec = 99; /* Hidden fields must remain unchanged. */
    policy.post_upload_rx_window_sec = 99;
    int id;
    assert(save_device_monitor_policy(a, &policy, &id) == 0 && id != original.id);
    assert(load_device_monitor_policy(a, &loaded, &generation) == 0);
    assert(loaded.auto_interval_sec == 600 && loaded.vibration_window_ms == 1000);
    assert(loaded.poll_interval_sec == original.poll_interval_sec && loaded.post_upload_rx_window_sec == original.post_upload_rx_window_sec);
    assert(generation == initial + 1 && owner_count() == 1);
    assert(load_device_monitor_policy(b, &other, &other_generation) == 0);
    assert(other.id == original.id && other.auto_interval_sec == original.auto_interval_sec && other_generation == initial);
    policy.id = original.id; /* A client cannot overwrite the shared default. */
    policy.vibration_window_ms = 1500;
    int same_id;
    assert(save_device_monitor_policy(a, &policy, &same_id) == 0 && same_id == id);
    assert(load_device_monitor_policy(a, &loaded, &generation) == 0 && generation == initial + 1 && owner_count() == 1);
    policy.raw_upload_every_cycles = 5;
    assert(save_device_monitor_policy(a, &policy, &same_id) == 0);
    assert(load_device_monitor_policy(a, &loaded, &generation) == 0 && generation == initial + 2);
    const char *nodes[] = {b};
    assert(save_monitor_policy(&policy, nodes, 1, &same_id) == -1); /* Cannot share A's owned policy. */
    assert(save_device_monitor_policy("missing", &policy, &same_id) == -1);
    close_database();
    assert(init_database() == 0);
    assert(load_device_monitor_policy(a, &loaded, &generation) == 0 && loaded.id == id && loaded.vibration_window_ms == 1500 && loaded.raw_upload_every_cycles == 5);
    assert(load_device_monitor_policy(b, &other, &other_generation) == 0 && other.id == original.id && other_generation == initial);
    policy = other;
    policy.auto_interval_sec = 1800;
    assert(save_device_monitor_policy(b, &policy, &same_id) == 0 && same_id != id && owner_count() == 2);
    assert(revoke_device_and_delete_data(a, 0x1234) == 0 && owner_count() == 1);
    assert(load_device_monitor_policy(a, &loaded, &generation) == -1);
    assert(load_device_monitor_policy(b, &other, &other_generation) == 0 && other.auto_interval_sec == 1800);
    close_database();
    assert(remove(GATEWAY_DATABASE_PATH) == 0);
    puts("node sampling: migration, isolation, ownership, generations, persistence and deletion PASS");
    return 0;
}
