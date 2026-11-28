#include <assert.h>
#include <stdio.h>
#include <string.h>
#define GATEWAY_DATABASE_PATH ":memory:"
#include "../gateway-core/src/db_manager.c"
#include "mqtt_store.h"

static const char *mac="01:02:03:04:05:06:07:08";
static const char *target="mqtt://localhost:1883|user|gw001|sparkplug_b:factory01";
static uint8_t raw[512*6];
static int32_t features[3][5]={{1000,2000,3000,4000,5000}};
static void seed(uint32_t batch) {
    assert(!mqtt_store_batch_scalar_status(g_db,mac,batch,15,0));
    assert(record_collection_temp(mac,batch,25)==1);
    assert(record_collection_mag(mac,batch,1,2,3)==1);
    assert(record_collection_sound(mac,batch,1,67)==1);
    assert(record_collection_sound(mac,batch,0,68)==1);
    assert(!save_vibration_batch(mac,batch,26667,512,raw,sizeof(raw),features));
}
static int reconcile(sqlite3_int64 quota) {
    return mqtt_store_reconcile(g_db,target,"gw001","factory01",quota);
}
static MqttStoreStats stats(void) {
    MqttStoreStats value;
    assert(!mqtt_store_backlog_stats(g_db,target,"gw001","factory01",&value));
    return value;
}
static int deny_writes(void *arg,int action,const char *a,const char *b,const char *db,const char *source) {
    (void)arg;(void)a;(void)b;(void)db;(void)source;
    return action==SQLITE_INSERT || action==SQLITE_UPDATE || action==SQLITE_DELETE?SQLITE_DENY:SQLITE_OK;
}
static void send_all(void) {
    MqttDelivery item;
    while(mqtt_store_next(g_db,target,&item)==1) {
        assert(mqtt_store_ack(g_db,item.id,item.waveform)>=0);
        mqtt_delivery_free(&item);
    }
}
int main(void) {
    assert(!init_database() && !insert_new_device(mac,"test"));
    assert(!mqtt_store_init(g_db) && !mqtt_store_target(g_db,target));
    assert(!stats().pending_batches);
    for(unsigned i=0;i<sizeof(raw);i++)raw[i]=(uint8_t)i;
    seed(1);seed(2);
    assert(!mqtt_store_pending(g_db,target));
    assert(sqlite3_set_authorizer(g_db,deny_writes,NULL)==SQLITE_OK);
    MqttStoreStats value=stats();
    assert(value.pending_batches==2 && value.pending_bytes>2*(sqlite3_int64)sizeof(raw));
    sqlite3_int64 all_bytes=value.pending_bytes;
    assert(sqlite3_set_authorizer(g_db,NULL,NULL)==SQLITE_OK);
    /* A count read does not queue sources, rewrite snapshots or clear dirty. */
    assert(!mqtt_store_pending(g_db,target));
    assert(reconcile(1)==2);assert(stats().pending_batches==2);
    assert(reconcile(1024*1024)==1);
    assert(stats().pending_batches==2 && stats().pending_bytes==all_bytes);
    assert(reconcile(1024*1024)==1);
    assert(stats().pending_batches==2 && stats().pending_bytes==all_bytes);
    MqttDelivery item;assert(mqtt_store_next(g_db,target,&item)==1 && !item.waveform);
    assert(!mqtt_store_ack(g_db,item.id,0));mqtt_delivery_free(&item);
    value=stats();assert(value.pending_batches==2 && value.pending_bytes<all_bytes);
    assert(mqtt_store_next(g_db,target,&item)==1 && item.waveform);
    sqlite3_int64 before_raw=value.pending_bytes;
    assert(mqtt_store_ack(g_db,item.id,1)==1);mqtt_delivery_free(&item);
    value=stats();assert(value.pending_batches==1 && value.pending_bytes==before_raw-(sqlite3_int64)sizeof(raw));
    send_all();assert(!stats().pending_batches && !stats().pending_bytes);
    /* Startup marks sources dirty; sent history still must not be recounted. */
    assert(!mqtt_store_init(g_db) && !mqtt_store_target(g_db,target));
    assert(!stats().pending_batches);
    assert(record_collection_temp(mac,1,26)==1);assert(stats().pending_batches==1);
    while(reconcile(1024*1024)==1){}
    assert(stats().pending_batches==1);send_all();assert(!stats().pending_batches);
    /* Multiple outstanding revisions are one source batch, with all remaining
     * bytes counted. A not-yet-queued newer revision adds bytes, not a batch. */
    seed(3);while(reconcile(1024*1024)==1){}
    sqlite3_int64 first_bytes=stats().pending_bytes;
    assert(record_collection_temp(mac,3,27)==1);
    value=stats();assert(value.pending_batches==1 && value.pending_bytes>first_bytes);
    sqlite3_int64 revisions_bytes=value.pending_bytes;
    while(reconcile(1024*1024)==1){}
    assert(stats().pending_batches==1 && stats().pending_bytes==revisions_bytes);
    send_all();assert(!stats().pending_batches);
    /* Incomplete local receives must not claim to be publishable batches. */
    assert(record_collection_temp(mac,4,25)==1);assert(!stats().pending_batches);
    assert(!mqtt_store_note(g_db,mac,5,MQTT_BATCH_CAPTURE));assert(!stats().pending_batches);
    seed(6);assert(!sqlite3_exec(g_db,"UPDATE devices SET status='offline';",NULL,NULL,NULL));
    assert(stats().pending_batches==1); // offline node backlog remains visible
    assert(!sqlite3_exec(g_db,"DELETE FROM mqtt_outbox;",NULL,NULL,NULL));
    MqttStoreStats alternate;
    assert(!mqtt_store_backlog_stats(g_db,"","gw001","factory01",&alternate));
    assert(alternate.pending_batches==4); // complete source history, destination not configured yet
    assert(mqtt_store_backlog_stats(NULL,target,"gw001","factory01",&alternate)==-1);
    assert(!alternate.pending_batches && !alternate.pending_bytes);
    close_database();
    puts("MQTT backlog PASS: disabled/unqueued/quota/offline sources, byte totals, partial sends, no duplicates, revisions, restart, incomplete exclusion, read-only stats");
    return 0;
}
