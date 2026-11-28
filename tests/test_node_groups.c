#include <assert.h>
#include <stdio.h>
#define GATEWAY_DATABASE_PATH "out/upload-scheduler-tests/node-groups-test.db"
#include "../gateway-core/src/db_manager.c"

int main(void)
{
    remove(GATEWAY_DATABASE_PATH); /* Dedicated test database; never production. */
    assert(init_database() == 0);
    const char *a = "01:02:03:04:05:06:07:08", *b = "11:12:13:14:15:16:17:18";
    assert(insert_new_device(a, "Sensor") == 0);
    assert(insert_new_device(b, "Sensor") == 0);
    int first = 0, second = 0;
    const char *one[] = {a};
    assert(save_node_group(&first, "同名组", "测试 'quoted'", one, 1) == 0);
    assert(save_node_group(&second, "同名组", "", NULL, 0) == 0);
    assert(first != second);
    MonitorPolicyRecord_t baseline, applied, loaded;
    uint32_t generation;
    assert(get_node_group_sampling(first, &loaded) == 0);
    assert(load_device_monitor_policy(a, &baseline, &generation) == 0);
    applied = baseline; applied.auto_interval_sec = 600; applied.vibration_window_ms = 1500;
    applied.raw_upload_every_cycles = 3; applied.sensor_enable_mask = 5;
    assert(update_device_monitoring(a, 1) == 0);
    int changed = -1, enabled;
    assert(apply_node_group_sampling(first, &applied, &changed) == 0 && changed == 1);
    assert(load_device_monitor_policy(a, &loaded, &generation) == 0 && loaded.auto_interval_sec == 600 && loaded.raw_upload_every_cycles == 3 && loaded.sensor_enable_mask == 5);
    assert(get_device_monitoring(a, &enabled) && enabled == 1);
    assert(load_device_monitor_policy(b, &loaded, &generation) == 0 && loaded.auto_interval_sec == baseline.auto_interval_sec);
    /* Never-applied group doesn't change settings of a new member. */
    const char *just_b[] = {b};
    assert(save_node_group(&second, "同名组", "", just_b, 1) == 0);
    assert(load_device_monitor_policy(b, &loaded, &generation) == 0 && loaded.auto_interval_sec == baseline.auto_interval_sec);
    assert(save_node_group(&second, "同名组", "", NULL, 0) == 0);
    const char *both[] = {a, b};
    assert(save_node_group(&first, "同名组", "", both, 2) == 0);
    assert(load_device_monitor_policy(b, &loaded, &generation) == 0 && loaded.auto_interval_sec == 600 && loaded.vibration_window_ms == 1500);
    assert(get_device_monitoring(b, &enabled) && enabled == 0);
    /* Editing membership doesn't reapply over a member's independent edits. */
    loaded.auto_interval_sec = 900;
    assert(save_device_monitor_policy(b, &loaded, NULL) == 0);
    assert(save_node_group(&first, "同名组", "", both, 2) == 0);
    assert(load_device_monitor_policy(b, &loaded, &generation) == 0 && loaded.auto_interval_sec == 900);
    applied.auto_interval_sec = 1200;
    assert(apply_node_group_sampling(first, &applied, &changed) == 0 && changed == 2);
    assert(load_device_monitor_policy(b, &loaded, &generation) == 0 && loaded.auto_interval_sec == 1200);
    /* Inject a failure after one member update: group snapshot and all nodes roll back. */
    assert(sqlite3_exec(g_db, "CREATE TRIGGER fail_group_policy BEFORE UPDATE ON monitor_policies WHEN OLD.owner_mac='11:12:13:14:15:16:17:18' BEGIN SELECT RAISE(ABORT,'injected'); END;", NULL, NULL, NULL) == SQLITE_OK);
    applied.auto_interval_sec = 1800;
    assert(apply_node_group_sampling(first, &applied, &changed) == -1);
    assert(get_node_group_sampling(first, &loaded) == 1 && loaded.auto_interval_sec == 1200);
    assert(load_device_monitor_policy(a, &loaded, &generation) == 0 && loaded.auto_interval_sec == 1200);
    assert(sqlite3_exec(g_db, "DROP TRIGGER fail_group_policy;", NULL, NULL, NULL) == SQLITE_OK);
    assert(save_node_group(&first, "同名组", "", one, 1) == 0);
    assert(save_node_group(&second, "不能抢成员", "", one, 1) == 1);
    char (*members)[32] = NULL;
    assert(get_node_group_members(first, &members) == 1 && !strcmp(members[0], a)); free(members);
    NodeGroupRecord_t *groups = NULL;
    assert(get_node_groups(&groups) == 2 && !strcmp(groups[1].name, "同名组")); free(groups);
    const char *unknown[] = {b, "missing"};
    assert(save_node_group(&first, "不应该保存", "", unknown, 2) == 2);
    assert(get_node_group_members(first, &members) == 1); free(members);
    const char *duplicate[] = {b, b};
    assert(save_node_group(&first, "不应该保存", "", duplicate, 2) == 2);
    assert(get_node_group_members(first, &members) == 1); free(members);
    assert(save_node_group(&first, "重命名", "", NULL, 0) == 0);
    assert(save_node_group(&second, "同名组", "", one, 1) == 0);
    close_database();
    assert(init_database() == 0);
    assert(get_node_groups(&groups) == 2); free(groups);
    assert(get_node_group_sampling(first, &loaded) == 1 && loaded.auto_interval_sec == 1200);
    assert(get_node_group_members(second, &members) == 1); free(members);
    assert(delete_device(a) == 0);
    assert(get_node_group_members(second, &members) == 0); free(members);
    /* Empty groups may apply a template; future members inherit it. */
    applied.auto_interval_sec = 600;
    assert(apply_node_group_sampling(first, &applied, &changed) == 0 && changed == 0);
    assert(save_node_group(&first, "模板", "", just_b, 1) == 0);
    assert(load_device_monitor_policy(b, &loaded, &generation) == 0 && loaded.auto_interval_sec == 600);
    assert(delete_node_group(first) == 0);
    assert(get_node_group_sampling(first, &loaded) == 0);
    assert(load_device_monitor_policy(b, &loaded, &generation) == 0 && loaded.auto_interval_sec == 600);
    const char *other[] = {b};
    assert(save_node_group(&second, "组", "", other, 1) == 0);
    assert(delete_node_group(second) == 0);
    assert(is_device_in_db(b));
    assert(get_node_group_members(second, &members) == 0); free(members);
    int invalid = 9999;
    assert(save_node_group(&invalid, "不存在", "", NULL, 0) == 2);
    assert(delete_node_group(9999) != 0);
    close_database();
    assert(remove(GATEWAY_DATABASE_PATH) == 0);
    puts("Node groups: duplicate names, exclusive membership, apply/inherit, no-edit reapply, atomic rollback, persistence, cascades PASS");
    return 0;
}
