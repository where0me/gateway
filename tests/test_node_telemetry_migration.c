#include <assert.h>
#include <stdio.h>
#define GATEWAY_DATABASE_PATH "out/vibration-tests/telemetry-migration-test.db"
#include "../gateway-core/src/db_manager.c"

int main(void)
{
    /* Isolated test database, never the gateway's data/gateway.db. */
    remove(GATEWAY_DATABASE_PATH);
    const char *mac = "01:02:03:04:05:06:07:08";
    assert(init_database() == 0);
    assert(insert_new_device(mac, "migration-test") == 0);
    assert(sqlite3_exec(g_db, "ALTER TABLE devices DROP COLUMN lqi;"
        "ALTER TABLE devices DROP COLUMN rssi;"
        "ALTER TABLE devices DROP COLUMN power_source;"
        "ALTER TABLE devices DROP COLUMN link_updated_at;", NULL, NULL, NULL) == SQLITE_OK);
    close_database();
    assert(init_database() == 0); // same upgrade path as a pre-telemetry database
    DeviceRecord_t *records = NULL;
    assert(get_all_devices(&records) == 1);
    assert(!strcmp(records[0].mac, mac) && records[0].lqi == -1 && records[0].power_source == 0);
    free(records);
    assert(update_device_link_telemetry(mac, 0, -57, 4) == 0);
    close_database();
    assert(init_database() == 0);
    assert(get_all_devices(&records) == 1);
    assert(records[0].lqi == 0 && records[0].rssi == -57 && records[0].power_source == 4 && records[0].link_updated_at > 0);
    free(records);
    assert(sqlite3_exec(g_db, "DELETE FROM devices;", NULL, NULL, NULL) == SQLITE_OK);
    assert(update_device_link_telemetry(mac, 100, -50, 4) == -1);
    assert(get_all_devices(&records) == 0);
    close_database();
    assert(remove(GATEWAY_DATABASE_PATH) == 0);
    puts("node telemetry: old-schema migration, restart persistence, unknown/zero LQI, deleted-node safety PASS");
    return 0;
}
