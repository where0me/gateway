#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#define GATEWAY_DATABASE_PATH ":memory:"
#include "../gateway-core/src/db_manager.c"
#include "cJSON.h"

struct mg_connection { int unused; };
struct mg_http_message { int unused; };
static char *api_response;
static void mg_http_reply(struct mg_connection *c, int status, const char *headers, const char *format, ...)
{
    (void)c; (void)headers;
    assert(status == 200 && !strcmp(format, "%s"));
    va_list args;
    va_start(args, format);
    api_response = strdup(va_arg(args, const char *));
    va_end(args);
    assert(api_response);
}
void api_handle_device_list(struct mg_connection *c, struct mg_http_message *hm);

int main(void)
{
    const char *one = "01:02:03:04:05:06:07:08";
    const char *two = "11:12:13:14:15:16:17:18";
    const char *three = "21:22:23:24:25:26:27:28";
    const char *empty = "31:32:33:34:35:36:37:38";
    LatestDeviceReadings_t readings;
    int32_t features[3][5] = {{1000,2000,3000,4000,5000},{0},{6000,7000,8000,9000,10000}};
    int32_t pending[3][5];
    for (unsigned axis = 0; axis < 3; axis++)
        for (unsigned field = 0; field < 5; field++) pending[axis][field] = INT32_MIN;
    assert(init_database() == 0);
    assert(insert_new_device(one, "one") == 0);
    assert(insert_new_device(two, "two") == 0);
    assert(insert_new_device(three, "three") == 0);
    assert(insert_new_device(empty, "empty") == 0);
    /* API fixtures represent already joined nodes, including an empty sensor. */
    assert(sqlite3_exec(g_db,"UPDATE devices SET join_confirmed=1;",NULL,NULL,NULL)==SQLITE_OK);
    assert(insert_new_device("41:42:43:44:45:46:47:48", "Unknown Device") == 0);
    assert(update_device_online("41:42:43:44:45:46:47:48","4321")==0);
    assert(save_device_interview("41:42:43:44:45:46:47:48","{\"state\":\"failed\"}")==0);
    assert(!get_latest_device_readings(empty, &readings));

    assert(upsert_collection_temp(one, 900, 24.5f) == 0);
    assert(upsert_collection_mag(one, 900, 0,-2,3) == 0);
    assert(upsert_collection_sound(one, 900, 1,0) == 0);
    assert(upsert_collection_sound(one, 900, 0,61) == 0);
    assert(save_vibration_batch(one,900,26667,10,NULL,0,features) == 0);
    assert(sqlite3_exec(g_db,"UPDATE collection_values SET timestamp=100;",NULL,NULL,NULL) == SQLITE_OK);
    assert(upsert_collection_temp(one, 3, 30) == 0); /* Batch IDs may decrease after reboot. */
    assert(upsert_collection_sound(one, 3, 1,67) == 0);
    assert(save_vibration_batch(one,3,26667,10,NULL,0,pending) == 0);
    assert(update_device_collection_batch_state(one,"capturing",0,3) == 0);
    // An older upload may finish after the next scheduled acquisition.
    assert(update_device_collection_batch_state(one,"complete",1,900) == 0);

    assert(upsert_collection_temp(two, 9,0) == 0);
    assert(save_vibration_batch(two,9,26667,10,NULL,0,features) == 0);
    assert(update_device_collection_state(two,"uploading",0) == 0);
    assert(upsert_collection_temp(three, 99,19) == 0);
    assert(save_vibration_batch(three,99,26667,10,NULL,0,features) == 0);
    assert(update_device_offline(three) == 0);
    assert(sqlite3_exec(g_db,"UPDATE devices SET temp='--',mag='--';",NULL,NULL,NULL) == SQLITE_OK);

    assert(get_latest_device_readings(one, &readings));
    assert(readings.present_mask == 63U);
    assert(readings.values[0] == 30 && readings.values[1] == 0 && readings.values[2] == -2);
    assert(readings.values[4] == 67 && readings.values[5] == 61);
    assert(readings.features_ready && readings.features_batch_id == 900);
    assert(!memcmp(readings.features, features, sizeof(features)));
    assert(get_latest_device_readings(two,&readings) && readings.values[0] == 0);
    assert(get_latest_device_readings(three,&readings) && readings.values[0] == 19);

    api_handle_device_list(NULL,NULL);
    cJSON *list = cJSON_Parse(api_response);
    assert(cJSON_GetArraySize(list) == 4);
    for (int i = 0; i < 4; i++) {
        cJSON *node = cJSON_GetArrayItem(list,i);
        const char *mac = cJSON_GetStringValue(cJSON_GetObjectItem(node,"mac"));
        cJSON *latest = cJSON_GetObjectItem(node,"latest_readings");
        if (!strcmp(mac,empty)) { assert(!latest); continue; }
        assert(cJSON_IsObject(latest));
        assert(cJSON_GetArraySize(cJSON_GetObjectItem(latest,"features")) == 3);
        double temp = cJSON_GetNumberValue(cJSON_GetObjectItem(latest,"temperature"));
        if (!strcmp(mac,one)) {
            assert(cJSON_GetNumberValue(cJSON_GetObjectItem(node,"collection_batch_id")) == 900);
            assert(cJSON_GetNumberValue(cJSON_GetObjectItem(node,"capture_batch_id")) == 3);
            assert(cJSON_IsTrue(cJSON_GetObjectItem(node,"capture_pending")));
            assert(temp == 30);
            assert(cJSON_GetNumberValue(cJSON_GetObjectItem(latest,"features_batch_id")) == 900);
            assert(cJSON_GetNumberValue(cJSON_GetObjectItem(latest,"analog_db")) == 61);
        } else if (!strcmp(mac,two)) assert(temp == 0);
        else {
            assert(temp == 19);
            assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(node,"status")),"offline"));
        }
    }
    cJSON_Delete(list); free(api_response);
    assert(update_device_collection_batch_state(one,"complete",1,3) == 0);
    DeviceRecord_t *records=NULL;int count=get_all_devices(&records);assert(count==5);
    for(int i=0;i<count;i++)if(!strcmp(records[i].mac,one))assert(!records[i].capture_pending && records[i].capture_batch_id==3);
    free(records);
    close_database();
    puts("PASS: latest per-sensor readings + actual device-list API: partial batches, queued descriptor, zero values, offline and node isolation");
    return 0;
}
