#include <assert.h>
#include <stdio.h>
#define GATEWAY_DATABASE_PATH "out/vibration-tests/node-description-test.db"
#include "../gateway-core/src/db_manager.c"

int main(void)
{
    remove(GATEWAY_DATABASE_PATH); /* isolated test database only */
    const char *mac = "01:02:03:04:05:06:07:08";
    assert(init_database() == 0);
    assert(insert_new_device(mac, "HY-Sensor") == 0);
    assert(update_device_online(mac, "1234") == 0);
    assert(sqlite3_exec(g_db, "ALTER TABLE devices DROP COLUMN description;", NULL, NULL, NULL) == SQLITE_OK);
    close_database();
    assert(init_database() == 0);
    const char *description = "车间节点 'quoted' \"description\" 100%\n第二行";
    assert(update_device_description(mac, description) == 0);
    assert(update_device_description("11:12:13:14:15:16:17:18", description) == 2);
    char long_description[770];
    memset(long_description, 'a', sizeof(long_description) - 1);
    long_description[sizeof(long_description) - 1] = '\0';
    assert(update_device_description(mac, long_description) == -1);
    close_database();
    assert(init_database() == 0);
    DeviceRecord_t *records = NULL;
    assert(get_all_devices(&records) == 1);
    assert(strcmp(records[0].description, description) == 0);
    assert(strcmp(records[0].short_addr, "1234") == 0);
    free(records);
    assert(update_device_description(mac, "") == 0);
    assert(get_all_devices(&records) == 1);
    assert(records[0].description[0] == '\0');
    free(records);
    close_database();
    assert(remove(GATEWAY_DATABASE_PATH) == 0);
    puts("Node metadata: legacy migration, description persistence/escaping, reset, length, unknown MAC and short address PASS");
    return 0;
}
