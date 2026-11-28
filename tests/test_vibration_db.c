#include <stdio.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#define GATEWAY_DATABASE_PATH ":memory:"
#include "../gateway-core/src/db_manager.c"

static int rows(const char *table) {
    sqlite3_stmt *stmt=NULL;char sql[128];
    snprintf(sql,sizeof(sql),"SELECT count(*) FROM %s",table);
    assert(sqlite3_prepare_v2(g_db,sql,-1,&stmt,NULL)==SQLITE_OK);
    assert(sqlite3_step(stmt)==SQLITE_ROW);
    int n=sqlite3_column_int(stmt,0);sqlite3_finalize(stmt);return n;
}
int main(void) {
    const char *mac="01:02:03:04:05:06:07:08";
    uint8_t descriptor[V2_BATCH_LEN]={0},raw[600]={0};
    int32_t features[3][5]={{0}};
    uint32_t auto_sec=0,analog_rate=0;
    uint16_t poll_sec=0,vibration_rate=0,digital_rate=0;
    uint16_t vibration_window=0,digital_window=0,analog_window=0,post_upload_rx_window=0;
    char channel_strategy[16]={0};int target_channel=0;
    assert(init_database()==0);
    assert(load_monitor_config(&auto_sec,&poll_sec,&vibration_rate,&digital_rate,
        &analog_rate,&vibration_window,&digital_window,&analog_window,
        &post_upload_rx_window)==0);
    assert(auto_sec==300 && poll_sec==5 && vibration_rate==26667 &&
        digital_rate==46875 && analog_rate==256000 && vibration_window==500 &&
        digital_window==1000 && analog_window==1000 && post_upload_rx_window==5);
    assert(save_monitor_config(300,2,26667,46875,256000,500,1000,1500,20)==0);
    assert(load_monitor_config(&auto_sec,&poll_sec,&vibration_rate,&digital_rate,
        &analog_rate,&vibration_window,&digital_window,&analog_window,
        &post_upload_rx_window)==0);
    assert(auto_sec==300 && poll_sec==2 && vibration_window==500 &&
        digital_window==1000 && analog_window==1500 && post_upload_rx_window==20);
    assert(load_zigbee_channel_config(channel_strategy,sizeof(channel_strategy),
        &target_channel)==0 && !strcmp(channel_strategy,"auto") && target_channel==0);
    assert(save_zigbee_channel_config("manual",25)==0);
    assert(load_zigbee_channel_config(channel_strategy,sizeof(channel_strategy),
        &target_channel)==0 && !strcmp(channel_strategy,"manual") && target_channel==25);
    assert(save_zigbee_channel_config("manual",27)==-1);
    assert(insert_new_device(mac,"test")==0);
    DeviceRecord_t *telemetry_records = NULL;
    assert(get_all_devices(&telemetry_records) == 1);
    assert(telemetry_records[0].lqi == -1 && telemetry_records[0].power_source == 0 &&
           telemetry_records[0].link_updated_at == 0);
    char telemetry_initial_status[16];
    snprintf(telemetry_initial_status, sizeof(telemetry_initial_status), "%s", telemetry_records[0].status);
    long long telemetry_initial_active = telemetry_records[0].last_active;
    free(telemetry_records);
    assert(update_device_link_telemetry(mac, 181, -46, 4) == 0);
    assert(get_all_devices(&telemetry_records) == 1);
    assert(telemetry_records[0].lqi == 181 && telemetry_records[0].rssi == -46 &&
           telemetry_records[0].power_source == 4 && telemetry_records[0].link_updated_at > 0);
    assert(!strcmp(telemetry_records[0].status, telemetry_initial_status) &&
           telemetry_records[0].last_active == telemetry_initial_active); // cache is not a heartbeat
    free(telemetry_records);
    assert(update_device_link_telemetry(mac, 0, -128, 0) == 0); // zero is a valid LQI
    assert(update_device_link_telemetry(mac, 256, -46, 4) == -1);
    assert(update_device_link_telemetry(mac, 181, -129, 4) == -1);
    assert(update_device_link_telemetry(mac, 181, -46, 7) == -1);
    assert(update_device_link_telemetry("missing-node", 181, -46, 4) == -1);
    assert(rows("devices") == 1); // telemetry never inserts an unauthorized node
    MonitorPolicyRecord_t policy={0};
    snprintf(policy.name,sizeof(policy.name),"machine group A");
    policy.auto_interval_sec=600;policy.poll_interval_sec=5;
    policy.vibration_rate_hz=26667;policy.digital_mic_rate_hz=46875;
    policy.analog_mic_rate_hz=256000;policy.vibration_window_ms=500;
    policy.digital_mic_window_ms=1000;policy.analog_mic_window_ms=1500;
    policy.raw_upload_every_cycles=3;policy.post_upload_rx_window_sec=10;
    const char *policy_nodes[]={mac};int policy_id=0;
    assert(save_monitor_policy(&policy,policy_nodes,1,&policy_id)==0 && policy_id>1);
    MonitorPolicyRecord_t loaded_policy={0};uint32_t generation=0;
    assert(load_device_monitor_policy(mac,&loaded_policy,&generation)==0);
    assert(loaded_policy.id==policy_id && loaded_policy.raw_upload_every_cycles==3 &&
        loaded_policy.post_upload_rx_window_sec==10 && generation==2);
    policy.vibration_window_ms=1000;
    assert(save_monitor_policy(&policy,policy_nodes,1,&policy_id)==0);
    assert(load_device_monitor_policy(mac,&loaded_policy,&generation)==0 && generation==2);
    policy.raw_upload_every_cycles=5;
    assert(save_monitor_policy(&policy,policy_nodes,1,&policy_id)==0);
    assert(load_device_monitor_policy(mac,&loaded_policy,&generation)==0 && generation==3);
    assert(delete_monitor_policy(policy_id)==0);
    assert(load_device_monitor_policy(mac,&loaded_policy,&generation)==0);
    assert(loaded_policy.is_default && generation==4);
    descriptor[0]=V2_VERSION;v2_w32(descriptor+1,1234);
    v2_w16(descriptor+5,26667);v2_w32(descriptor+7,100);v2_w16(descriptor+11,512);
    v2_w32(descriptor+13,v2_crc(raw,sizeof(raw)));
    assert(vibration_batch_matches(mac,descriptor)==0);
    assert(save_vibration_batch(mac,1234,26667,100,NULL,0,features)==0);
    assert(vibration_batch_matches(mac,descriptor)==0);
    assert(sqlite3_exec(g_db,"PRAGMA query_only=ON",NULL,NULL,NULL)==SQLITE_OK);
    assert(save_vibration_batch(mac,1234,26667,100,raw,sizeof(raw),features)==-1);
    assert(vibration_batch_matches(mac,descriptor)==0);
    assert(sqlite3_exec(g_db,"PRAGMA query_only=OFF",NULL,NULL,NULL)==SQLITE_OK);
    assert(save_vibration_batch(mac,1234,26667,100,raw,sizeof(raw),features)==0);
    assert(vibration_batch_matches(mac,descriptor)==1);
    assert(save_vibration_batch(mac,1234,26667,100,NULL,0,features)==0);
    assert(vibration_batch_matches(mac,descriptor)==1 && rows("vibration_batches")==1);
    descriptor[13]^=1;assert(vibration_batch_matches(mac,descriptor)==-1);descriptor[13]^=1;
    descriptor[17]^=1;assert(vibration_batch_matches(mac,descriptor)==-1);descriptor[17]^=1;
    /* Waveform descriptor first, final summary later; restart must accept it. */
    int32_t pending_features[3][5];
    for(unsigned a=0;a<3;a++)for(unsigned f=0;f<5;f++) {
        pending_features[a][f]=INT32_MIN;
        v2_w32(descriptor+17+(a*5+f)*4,UINT32_C(0x80000000));
    }
    assert(vibration_batch_matches(mac,descriptor)==1);
    assert(save_vibration_batch(mac,1234,26667,100,raw,sizeof(raw),pending_features)==0);
    uint32_t latest_id,latest_rate,latest_count;
    void *latest_raw=NULL;size_t latest_bytes=0;long long latest_time=0;
    int32_t latest_features[3][5];
    assert(get_latest_vibration_batch(mac,&latest_id,&latest_rate,&latest_count,
        &latest_raw,&latest_bytes,latest_features,&latest_time)==1);
    assert(!memcmp(latest_features,features,sizeof(features)));free(latest_raw);
    assert(vibration_batch_matches(mac,descriptor)==1);
    assert(revoke_device_and_delete_data(mac,0x4321)==0);
    assert(rows("devices")==0 && rows("vibration_batches")==0);
    RevokedNodeRecord_t revoked={0};
    assert(get_revoked_node_by_mac(mac,&revoked)==1);
    assert(revoked.has_short_addr && revoked.last_short_addr==0x4321);
    assert(!strcmp(revoked.state,"pending"));
    assert(revoked.last_leave_at==0 && revoked.retry_count==0);
    assert(mark_revoked_leave_attempt(mac,0x4321)==0);
    assert(get_revoked_node_by_short(0x4321,&revoked)==1);
    assert(!strcmp(revoked.state,"sending") && revoked.retry_count==1);
    assert(mark_revoked_node_guard(mac,0x4321)==0);
    assert(get_revoked_node_by_mac(mac,&revoked)==1 && !strcmp(revoked.state,"guard"));
    assert(release_revoked_short_for_other_mac("11:12:13:14:15:16:17:18",0x4321)==0);
    assert(get_revoked_node_by_short(0x4321,&revoked)==0);
    assert(clear_revoked_node(mac)==0 && get_revoked_node_by_mac(mac,&revoked)==0);
    assert(vibration_batch_matches(mac,descriptor)==0);
    assert(save_vibration_batch(mac,1234,26667,100,raw,sizeof(raw),features)==-1);
    ChannelOperationRecord_t operation={0},loaded_operation={0};
    ChannelOperationNodeRecord_t operation_nodes[2]={0},*loaded_nodes=NULL;
    int loaded_node_count=0;char snapshot_error[160]={0};
    snprintf(operation.state,sizeof(operation.state),"waiting");
    operation.strategy=1;operation.target_channel=20;operation.current_channel=11;
    operation.waiting_for_nodes=1;operation.node_count=2;operation.prepared_count=1;
    snprintf(operation.message,sizeof(operation.message),"waiting for one node");
    snprintf(operation_nodes[0].mac,sizeof(operation_nodes[0].mac),"01:02:03:04:05:06:07:08");
    operation_nodes[0].short_addr=0x1234;operation_nodes[0].state=1;
    snprintf(operation_nodes[1].mac,sizeof(operation_nodes[1].mac),"11:12:13:14:15:16:17:18");
    operation_nodes[1].short_addr=0x5678;operation_nodes[1].state=0;
    assert(save_channel_operation_snapshot(&operation,operation_nodes,2,
                                           snapshot_error,sizeof(snapshot_error))==0);
    assert(load_channel_operation_record(&loaded_operation)==0);
    assert(load_channel_operation_nodes(&loaded_nodes,&loaded_node_count)==0);
    assert(loaded_operation.target_channel==20 && loaded_operation.node_count==2 &&
           loaded_operation.prepared_count==1 && loaded_node_count==2);
    free(loaded_nodes);
    close_database();
    puts("database: sampling policies, channel, revocation guard and cascade removal PASS");
    return 0;
}
