#include <assert.h>
#include <stdio.h>
#define GATEWAY_DATABASE_PATH "capture-times-test.db"
#include "../gateway-core/src/db_manager.c"

int main(void)
{
    remove(GATEWAY_DATABASE_PATH);
    assert(init_database() == 0);
    assert(insert_new_device("one", "one") == 0);
    assert(insert_new_device("two", "two") == 0);
    assert(get_collection_capture_time("one", 100) == 0);
    assert(update_device_collection_batch_state("one", "capturing", 0, 100) == 0);
    assert(get_collection_capture_time("one", 100) > 0);
    /* A known start predates every later upload/repair/completion update. */
    assert(sqlite3_exec(g_db, "UPDATE collection_capture_times SET captured_at=1000;", 0, 0, 0) == SQLITE_OK);
    assert(update_device_collection_batch_state("one", "capturing", 0, 100) == 0);
    assert(update_device_collection_batch_state("one", "waiting", 0, 100) == 0);
    assert(upsert_collection_temp("one", 100, 24.0f) == 0);
    int32_t features[3][5] = {{0}};
    int16_t samples[6] = {0};
    assert(save_vibration_batch("one", 100, 1000, 2, samples, sizeof(samples), features) == 0);
    assert(update_device_collection_batch_state("one", "complete", 1, 100) == 0);
    assert(get_collection_capture_time("one", 100) == 1000);
    assert(get_collection_capture_time("two", 100) == 0);
    assert(get_collection_capture_time("one", 101) == 0);
    /* Backlog arrives out of order: history follows acquisition, not arrival. */
    assert(update_device_collection_batch_state("one", "capturing", 0, 101) == 0);
    assert(sqlite3_exec(g_db, "UPDATE collection_capture_times SET captured_at=2000 WHERE batch_id=101;", 0, 0, 0) == SQLITE_OK);
    assert(upsert_collection_temp("one", 101, 25.0f) == 0);
    assert(sqlite3_exec(g_db, "UPDATE collection_values SET timestamp=CASE batch_id WHEN 100 THEN 9000 ELSE 8000 END;", 0, 0, 0) == SQLITE_OK);
    CollectionHistoryRecord_t *rows = NULL;
    assert(get_collection_history("one", "temperature", 30, &rows) == 2);
    assert(rows[0].batch_id == 100 && rows[0].captured_at == 1000 && rows[0].timestamp == 9000);
    assert(rows[1].batch_id == 101 && rows[1].captured_at == 2000);
    free(rows);
    /* Legacy rows remain unknown rather than acquiring a fabricated time. */
    assert(upsert_collection_temp("two", 100, 26.0f) == 0);
    assert(get_collection_history("two", "temperature", 30, &rows) == 1);
    assert(rows[0].captured_at == 0);
    free(rows);
    uint32_t deadline,delay;
    assert(get_sampling_resume_plan("one",300,2125000,&deadline,&delay)==1);
    assert(delay==175000 && deadline==2300); /* Last capture at 2000 s. */
    assert(get_sampling_resume_plan("one",300,2300000,&deadline,&delay)==1 && delay==300000);
    assert(get_sampling_resume_plan("one",300,3100000,&deadline,&delay)==1 && delay==100000);
    assert(get_sampling_resume_plan("one",600,2125000,&deadline,&delay)==1 && delay==475000);
    assert(get_sampling_resume_plan("two",300,4000000,&deadline,&delay)==1 && delay==300000);
    assert(get_sampling_resume_plan("two",300,4100000,&deadline,&delay)==1 && delay==200000);
    assert(get_sampling_resume_plan("missing",300,4000000,&deadline,&delay)==0);
    assert(get_sampling_resume_plan("one",59,4000000,&deadline,&delay)==0);
    assert(get_sampling_resume_plan("one",60,2125000,&deadline,&delay)==1 &&
           deadline==2180 && delay==55000);
    assert(save_monitor_config(60,5,26667,46875,256000,500,1000,1000,5)==0);
    MonitorPolicyRecord_t policy={0};
    uint32_t generation;
    assert(load_device_monitor_policy("one",&policy,&generation)==0);
    policy.auto_interval_sec=60;
    int policy_id=0;
    assert(save_device_monitor_policy("one",&policy,&policy_id)==0);
    assert(update_device_expected_interval("one",60)==0);
    close_database();
    assert(init_database() == 0);
    assert(get_collection_capture_time("one", 100) == 1000);
    assert(get_sampling_resume_plan("two",300,4200000,&deadline,&delay)==1 && delay==100000);
    assert(load_device_monitor_policy("one",&policy,&generation)==0 && policy.auto_interval_sec==60);
    uint32_t period,analog_rate;
    uint16_t poll,rate,digital_rate,vibration,digital,analog,receive;
    assert(load_monitor_config(&period,&poll,&rate,&digital_rate,&analog_rate,
        &vibration,&digital,&analog,&receive)==0 && period==60);
    DeviceRecord_t *devices=NULL;
    int count=get_all_devices(&devices),found=0;
    for (int i=0;i<count;i++)
        if (!strcmp(devices[i].mac,"one")) found=devices[i].expected_interval_sec==60;
    assert(found);
    free(devices);
    assert(update_device_collection_batch_state("one", "capturing", 0, 100) == 0);
    assert(get_collection_capture_time("one", 100) == 1000);
    close_database();
    remove(GATEWAY_DATABASE_PATH);
    puts("Capture times: immutable per batch, upload independence, history ordering, isolation, unknown legacy and restart PASS");
    return 0;
}
