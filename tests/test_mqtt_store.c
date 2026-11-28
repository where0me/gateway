#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define GATEWAY_DATABASE_PATH ":memory:"
#include "../gateway-core/src/db_manager.c"
#include "mqtt_store.h"
#include "cJSON.h"
static const char *mac="01:02:03:04:05:06:07:08",*target="test-broker|gateway|gw001|sparkplug_b:zigbee";
static uint8_t raw[512*6];
static int32_t features[3][5]={{1000,2000,3000,4000,5000},{2000,3000,4000,5000,6000},{3000,4000,5000,6000,7000}};
static void scalars(uint32_t batch) {
    assert(record_collection_temp(mac,batch,0)==1);
    assert(record_collection_mag(mac,batch,1,-2,3)==1);
    assert(record_collection_sound(mac,batch,1,67)==1);
    assert(record_collection_sound(mac,batch,0,68)==1);
}
static int reconcile(void) {return mqtt_store_reconcile(g_db,target,"gw001","zigbee",1024*1024);}
static int scalar_query(const char *sql) {
    sqlite3_stmt *s=NULL;assert(sqlite3_prepare_v2(g_db,sql,-1,&s,NULL)==SQLITE_OK);
    assert(sqlite3_step(s)==SQLITE_ROW);int result=sqlite3_column_int(s,0);sqlite3_finalize(s);return result;
}
int main(int argc,char **argv) {
    assert(argc==2);remove(argv[1]);
    assert(init_database()==0 && insert_new_device(mac,"test")==0);
    for(unsigned i=0;i<sizeof(raw);i++)raw[i]=(uint8_t)(i*17);
    /* Old full waveform is provably complete; old absent waveform is ambiguous. */
    scalars(1);assert(save_vibration_batch(mac,1,26667,512,raw,sizeof(raw),features)==0);
    scalars(2);assert(save_vibration_batch(mac,2,26667,512,NULL,0,features)==0);
    /* Upgrade an existing pre-capture-status table without losing old rows. */
    assert(sqlite3_exec(g_db,"CREATE TABLE mqtt_batch_state(mac TEXT NOT NULL,batch_id INTEGER NOT NULL,"
        "sensor_mask INTEGER,needs_vibration INTEGER NOT NULL DEFAULT 0,raw_started INTEGER NOT NULL DEFAULT 0,"
        "summary_seen INTEGER NOT NULL DEFAULT 0,node_complete INTEGER NOT NULL DEFAULT 0,"
        "dirty INTEGER NOT NULL DEFAULT 1,reason TEXT NOT NULL DEFAULT 'waiting',PRIMARY KEY(mac,batch_id));"
        "INSERT INTO mqtt_batch_state(mac,batch_id,needs_vibration) VALUES('01:02:03:04:05:06:07:08',1,1);",
        NULL,NULL,NULL)==SQLITE_OK);
    assert(mqtt_store_init(g_db)==0 && mqtt_store_target(g_db,target)==0);
    assert(scalar_query("SELECT scalar_unavailable FROM mqtt_batch_state WHERE batch_id=1;")==0);
    assert(reconcile()==1 && reconcile()==0);
    assert(scalar_query("SELECT count(*) FROM mqtt_batch_state WHERE reason='legacy_completion_unknown';")==1);
    assert(mqtt_store_pending(g_db,target)==1);
    /* Upgrade only changes an internal topic hint, not measurement identity. */
    assert(sqlite3_exec(g_db,"UPDATE mqtt_outbox SET canonical=replace(canonical,'spBv1.0/zigbee/DDATA/gw001/0102030405060708','zigbee/gw001/nodes/0102030405060708/batches/1/waveform');",NULL,NULL,NULL)==SQLITE_OK);
    assert(!mqtt_store_target(g_db,target) && reconcile()==0 && mqtt_store_pending(g_db,target)==1);
    MqttDelivery item;
    assert(mqtt_store_next(g_db,target,&item)==1 && !item.waveform && item.batch==1);
    sqlite3_int64 id=item.id;
    assert(!strcmp(item.topic,"spBv1.0/zigbee/DDATA/gw001/0102030405060708"));
    cJSON *json=cJSON_Parse(item.payload);assert(json);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(json,"collection_complete")));
    assert(cJSON_IsNull(cJSON_GetObjectItem(json,"collected_at")));
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(json,"measurements"),"temperature_c")->valuedouble==0);
    cJSON *wave=cJSON_GetObjectItem(json,"waveform");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(wave,"available")) && cJSON_GetObjectItem(wave,"bytes")->valueint==sizeof(raw));
    assert(!strcmp(cJSON_GetObjectItem(wave,"encoding")->valuestring,"xyz-int16-le"));
    assert(!strcmp(cJSON_GetObjectItem(wave,"topic")->valuestring,item.topic));
    cJSON_Delete(json);mqtt_delivery_free(&item);
    assert(mqtt_store_ack(g_db,id,1)==-1); /* cannot acknowledge raw before JSON */
    assert(sqlite3_exec(g_db,"CREATE TRIGGER reject_ack BEFORE UPDATE OF json_acked ON mqtt_outbox BEGIN SELECT RAISE(ABORT,'test ACK failure'); END;",NULL,NULL,NULL)==SQLITE_OK);
    assert(mqtt_store_ack(g_db,id,0)==-1 && mqtt_store_next(g_db,target,&item)==1 && !item.waveform);
    mqtt_delivery_free(&item);
    assert(sqlite3_exec(g_db,"DROP TRIGGER reject_ack;",NULL,NULL,NULL)==SQLITE_OK);
    assert(mqtt_store_ack(g_db,id,0)==0);
    MqttStoreStats stats;
    assert(mqtt_store_stats(g_db,target,&stats)==0 && stats.pending_batches==1 && stats.pending_bytes==sizeof(raw) && !stats.last_confirmed_at);
    assert(mqtt_store_next(g_db,target,&item)==1 && item.waveform && item.id==id && item.length==sizeof(raw));
    assert(!memcmp(item.payload,raw,sizeof(raw)));mqtt_delivery_free(&item);
    /* Snapshot outstanding payloads to disk; reopening preserves exact retry. */
    sqlite3 *copy=NULL;assert(sqlite3_open(argv[1],&copy)==SQLITE_OK);
    sqlite3_backup *backup=sqlite3_backup_init(copy,"main",g_db,"main");assert(backup);
    assert(sqlite3_backup_step(backup,-1)==SQLITE_DONE && sqlite3_backup_finish(backup)==SQLITE_OK);
    sqlite3_close(copy);assert(sqlite3_open(argv[1],&copy)==SQLITE_OK);
    assert(mqtt_store_init(copy)==0 && mqtt_store_target(copy,target)==0);
    assert(mqtt_store_next(copy,target,&item)==1 && item.waveform && item.id==id);
    assert(!memcmp(item.payload,raw,sizeof(raw)));mqtt_delivery_free(&item);
    assert(mqtt_store_ack(copy,id,1)==1 && mqtt_store_pending(copy,target)==0);sqlite3_close(copy);remove(argv[1]);
    assert(mqtt_store_ack(g_db,id,1)==1);
    assert(mqtt_store_stats(g_db,target,&stats)==0 && !stats.pending_batches && !stats.pending_bytes && stats.last_confirmed_at>0);
    assert(scalar_query("SELECT raw_payload IS NULL AND json_payload IS NULL FROM mqtt_outbox WHERE id=1;")==1);
    assert(mqtt_store_note(g_db,mac,1,MQTT_BATCH_SUMMARY)==0 && reconcile()==0); /* duplicate evidence != duplicate task */
    assert(mqtt_store_note(g_db,mac,2,MQTT_BATCH_SUMMARY)==0 && reconcile()==1);
    assert(mqtt_store_next(g_db,target,&item)==1 && item.batch==2);
    id=item.id;json=cJSON_Parse(item.payload);assert(json);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetObjectItem(json,"waveform"),"available")));
    cJSON_Delete(json);mqtt_delivery_free(&item);assert(mqtt_store_ack(g_db,id,0)==1);
    /* Disabled channels are missing, not zero, using the captured policy. */
    assert(mqtt_store_policy(g_db,mac,0)==0 && mqtt_store_note(g_db,mac,3,MQTT_BATCH_CAPTURE)==0);
    assert(mqtt_store_policy(g_db,mac,7)==0); /* must not rewrite this batch's snapshot */
    assert(record_collection_temp(mac,3,20)==1 && record_collection_mag(mac,3,0,0,0)==1);
    assert(reconcile()==0 && mqtt_store_pending(g_db,target)==0);
    assert(mqtt_store_note(g_db,mac,3,MQTT_BATCH_COMPLETE)==0 && reconcile()==1);
    assert(mqtt_store_next(g_db,target,&item)==1 && item.batch==3);
    id=item.id;json=cJSON_Parse(item.payload);
    assert(cJSON_IsNull(cJSON_GetObjectItem(cJSON_GetObjectItem(json,"measurements"),"digital_mic_db_spl")));
    cJSON_Delete(json);mqtt_delivery_free(&item);assert(mqtt_store_ack(g_db,id,0)==1);
    /* Partial, pending features, and raw-started-but-empty batches stay pending. */
    scalars(4);assert(save_vibration_batch(mac,4,26667,512,raw,12,features)==0);
    assert(mqtt_store_note(g_db,mac,4,MQTT_BATCH_COMPLETE)==0 && reconcile()==0);
    scalars(5);int32_t pending[3][5];for(unsigned i=0;i<15;i++)pending[i/5][i%5]=INT32_MIN;
    assert(save_vibration_batch(mac,5,26667,512,raw,sizeof(raw),pending)==0 && reconcile()==0);
    scalars(6);assert(save_vibration_batch(mac,6,26667,512,NULL,0,features)==0);
    assert(mqtt_store_note(g_db,mac,6,MQTT_BATCH_RAW)==0 && mqtt_store_note(g_db,mac,6,MQTT_BATCH_SUMMARY)==0);
    assert(reconcile()==0 && mqtt_store_pending(g_db,target)==0);
    /* Outbox failure must roll back both parts; retry/quota must not lose dirty source. */
    scalars(7);assert(save_vibration_batch(mac,7,26667,512,raw,sizeof(raw),features)==0);
    assert(sqlite3_exec(g_db,"CREATE TRIGGER reject_mqtt BEFORE UPDATE OF json_payload ON mqtt_outbox BEGIN SELECT RAISE(ABORT,'test failure'); END;",NULL,NULL,NULL)==SQLITE_OK);
    assert(reconcile()==-1 && mqtt_store_pending(g_db,target)==0);
    assert(sqlite3_exec(g_db,"DROP TRIGGER reject_mqtt;",NULL,NULL,NULL)==SQLITE_OK);
    assert(mqtt_store_reconcile(g_db,target,"gw001","zigbee",1)==2 && mqtt_store_pending(g_db,target)==0);
    assert(reconcile()==1 && mqtt_store_pending(g_db,target)==1);
    assert(mqtt_store_next(g_db,target,&item)==1 && item.batch==7);id=item.id;mqtt_delivery_free(&item);
    /* Lost ACK leaves same row pending; a different broker must not receive it. */
    assert(mqtt_store_next(g_db,target,&item)==1 && item.id==id);mqtt_delivery_free(&item);
    assert(mqtt_store_next(g_db,"different-broker",&item)==0);
    assert(mqtt_store_ack(g_db,id,0)==0);
    /* Deleting the source node cannot destroy an already queued, unconfirmed snapshot. */
    assert(delete_device(mac)==0);
    assert(mqtt_store_next(g_db,target,&item)==1 && item.waveform && item.length==sizeof(raw));
    assert(!memcmp(item.payload,raw,sizeof(raw)));mqtt_delivery_free(&item);assert(mqtt_store_ack(g_db,id,1)==1);
    assert(mqtt_store_pending(g_db,target)==0);
    /* Switching A -> B -> A must revisit corrections exported only to B. */
    assert(insert_new_device(mac,"test")==0);scalars(8);
    assert(save_vibration_batch(mac,8,26667,512,raw,sizeof(raw),features)==0 && reconcile()==1);
    assert(mqtt_store_next(g_db,target,&item)==1);id=item.id;mqtt_delivery_free(&item);
    assert(mqtt_store_ack(g_db,id,0)==0 && mqtt_store_ack(g_db,id,1)==1);
    const char *other_target="broker-b|gateway|gw001|sparkplug_b:zigbee";
    assert(mqtt_store_target(g_db,other_target)==0 && mqtt_store_reconcile(g_db,other_target,"gw001","zigbee",1024*1024)==1);
    assert(record_collection_temp(mac,8,99)==1 && mqtt_store_reconcile(g_db,other_target,"gw001","zigbee",1024*1024)==1);
    assert(mqtt_store_target(g_db,target)==0 && reconcile()==1 && mqtt_store_pending(g_db,target)==1);
    /* Late capture metadata supersedes an unreceived/newer downlink policy. */
    assert(mqtt_store_policy(g_db,mac,7)==0 && mqtt_store_note(g_db,mac,99,MQTT_BATCH_CAPTURE)==0);
    assert(scalar_query("SELECT sensor_mask FROM mqtt_batch_state WHERE batch_id=99;")==7);
    assert(mqtt_store_batch_policy(g_db,mac,99,1)==0);
    assert(mqtt_store_policy(g_db,mac,7)==0 && mqtt_store_note(g_db,mac,99,MQTT_BATCH_RAW)==0);
    assert(scalar_query("SELECT sensor_mask FROM mqtt_batch_state WHERE batch_id=99;")==1);
    int frozen=scalar_query("SELECT COALESCE(sensor_mask,-1) FROM mqtt_batch_state WHERE batch_id=8;");
    assert(mqtt_store_batch_policy(g_db,mac,8,1)==0);
    assert(scalar_query("SELECT COALESCE(sensor_mask,-1) FROM mqtt_batch_state WHERE batch_id=8;")==frozen);
    /* Declared capture failures do not block the available waveform or invent zero values. */
    for(unsigned unavailable=1;unavailable<=15;unavailable++) {
        uint32_t batch=100+unavailable;
        assert(mqtt_store_batch_scalar_status(g_db,mac,batch,15,unavailable)==0);
        if(!(unavailable&1))assert(record_collection_temp(mac,batch,0)==1);
        if(!(unavailable&2))assert(record_collection_mag(mac,batch,0,-2,3)==1);
        if(!(unavailable&4))assert(record_collection_sound(mac,batch,1,0)==1);
        if(!(unavailable&8))assert(record_collection_sound(mac,batch,0,68)==1);
        assert(save_vibration_batch(mac,batch,26667,512,raw,sizeof(raw),features)==0);
        assert(mqtt_store_note(g_db,mac,batch,MQTT_BATCH_RAW)==0);
        if(unavailable==1)assert(mqtt_store_init(g_db)==0); /* persistent across startup */
        for(unsigned attempt=0;attempt<32;attempt++)assert(reconcile()>=0);
        sqlite3_stmt *row=NULL;
        assert(sqlite3_prepare_v2(g_db,"SELECT json_payload FROM mqtt_outbox WHERE mac=? AND batch_id=? AND target=?;",-1,&row,NULL)==SQLITE_OK);
        sqlite3_bind_text(row,1,mac,-1,SQLITE_STATIC);sqlite3_bind_int64(row,2,batch);sqlite3_bind_text(row,3,target,-1,SQLITE_STATIC);
        assert(sqlite3_step(row)==SQLITE_ROW);json=cJSON_Parse((const char *)sqlite3_column_text(row,0));sqlite3_finalize(row);assert(json);
        cJSON *capture=cJSON_GetObjectItem(json,"capture_status"),*values=cJSON_GetObjectItem(json,"measurements");
        assert(cJSON_IsFalse(cJSON_GetObjectItem(capture,"complete")));
        unsigned expected_count=0;for(unsigned bit=0;bit<4;bit++)expected_count+=(unavailable>>bit)&1U;
        assert(cJSON_GetArraySize(cJSON_GetObjectItem(capture,"unavailable_measurements"))==(int)expected_count);
        const char *names[]={"temperature_c","magnetic_ut","digital_mic_db_spl","ultrasonic_mic_db_spl"};
        for(unsigned item_index=0;item_index<4;item_index++) {
            cJSON *value=cJSON_GetObjectItem(values,names[item_index]);
            if(item_index==1)value=cJSON_GetObjectItem(value,"x");
            assert(cJSON_IsNull(value)==!!(unavailable&(1U<<item_index)));
        }
        assert(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(json,"waveform"),"available")));
        assert(cJSON_GetObjectItem(json,"received_at")->valuedouble>0);cJSON_Delete(json);
        /* Once exported, a later API call must not change the stored status. */
        assert(mqtt_store_batch_scalar_status(g_db,mac,batch,15,0)==0);
        char query[160];snprintf(query,sizeof(query),"SELECT scalar_unavailable FROM mqtt_batch_state WHERE batch_id=%u;",batch);
        assert(scalar_query(query)==(int)unavailable);
    }
    assert(mqtt_store_batch_scalar_status(g_db,mac,999,3,4)==-1);
    /* Retired JSON rows remain intact but do not consume Sparkplug quota. */
    assert(sqlite3_exec(g_db,"UPDATE mqtt_outbox SET json_acked=1,raw_acked=1,json_payload=NULL,raw_payload=NULL;"
        "INSERT INTO mqtt_outbox(target,mac,batch_id,canonical,json_payload,raw_payload,json_topic,raw_topic,raw_acked) "
        "VALUES('legacy|gateway|gw001|zigbee','01:02:03:04:05:06:07:08',998,'{}','{}',zeroblob(2097152),'legacy/json','legacy/raw',0);",
        NULL,NULL,NULL)==SQLITE_OK);
    scalars(1000);assert(!save_vibration_batch(mac,1000,26667,512,raw,sizeof(raw),features));
    assert(reconcile()==1 && mqtt_store_pending(g_db,target)==1);
    assert(scalar_query("SELECT length(raw_payload) FROM mqtt_outbox WHERE target='legacy|gateway|gw001|zigbee';")==2097152);
    close_database();
    puts("MQTT store: snapshot/raw integrity, restart, SQL rollback, quota, legacy archive isolation, routing, topic migration without replay PASS");
    return 0;
}
