#include <assert.h>
#include <stdio.h>
#define GATEWAY_DATABASE_PATH "out/upload-scheduler-tests/monitor-switch-test.db"
#include "../gateway-core/src/db_manager.c"
#include "monitor_standby.h"
int main(void)
{
    remove(GATEWAY_DATABASE_PATH);
    assert(init_database() == 0);
    const char *mac = "01:02:03:04:05:06:07:08";
    int enabled = -1;
    assert(insert_new_device(mac, "Sensor") == 0);
    assert(get_device_monitoring(mac, &enabled) && enabled == 0);
    /* Idle status refreshes genuine uplink telemetry/activity, not the desired
       monitoring switch, previous waveform, or collection completion time. */
    assert(update_device_online(mac, "1234") == 0);
    assert(update_device_collection_state(mac, "idle", 0) == 0);
    assert(update_device_link_telemetry(mac, 149, -46, 4) == 0);
    DeviceRecord_t *records = NULL;
    assert(get_all_devices(&records) == 1);
    assert(records[0].lqi == 149 && records[0].rssi == -46 && records[0].power_source == 4);
    assert(records[0].last_active > 0 && records[0].link_updated_at > 0);
    assert(records[0].last_collection == 0);
    free(records);
    assert(get_device_monitoring(mac, &enabled) && enabled == 0);
    close_database(); assert(init_database() == 0);
    assert(get_device_monitoring(mac, &enabled) && enabled == 0);
    assert(update_device_monitoring(mac, 1) == 0);
    close_database(); assert(init_database() == 0);
    assert(get_device_monitoring(mac, &enabled) && enabled == 1);
    assert(insert_new_device(mac, "Sensor") == 0); /* repeated authorization preserves choice */
    assert(get_device_monitoring(mac, &enabled) && enabled == 1);
    assert(update_device_monitoring(mac, 0) == 0);
    close_database(); assert(init_database() == 0);
    assert(get_device_monitoring(mac, &enabled) && enabled == 0);
    close_database(); assert(remove(GATEWAY_DATABASE_PATH) == 0);
    MonitorStandby s={.random=1};int awake;
    uint32_t delay=monitor_standby_delay(&s,100,&awake);
    assert(awake && delay>=3000 && delay<=10000);
    assert(monitor_standby_delay(&s,599100,&awake)==1000 && awake);
    delay=monitor_standby_delay(&s,600100,&awake);
    assert(!awake && delay>=285000 && delay<=315000);
    s=(MonitorStandby){0};monitor_standby_enabled(&s);
    delay=monitor_standby_delay(&s,200,&awake);
    assert(!awake && delay>=285000 && delay<=315000);
    puts("Monitoring desired switch persistence/default OFF and actual node standby timer policy PASS");
    return 0;
}
