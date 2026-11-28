#ifndef MQTT_STORE_H
#define MQTT_STORE_H
#include <sqlite3.h>
#include <stdint.h>
#include <stddef.h>

enum MqttBatchEvent { MQTT_BATCH_CAPTURE=1, MQTT_BATCH_VIBRATION, MQTT_BATCH_RAW,
                      MQTT_BATCH_SUMMARY, MQTT_BATCH_COMPLETE };
typedef struct {
    sqlite3_int64 id;
    int waveform;
    char mac[24],topic[512];
    uint32_t batch;
    void *payload;
    size_t length;
} MqttDelivery;
int mqtt_store_init(sqlite3 *db);
int mqtt_store_policy(sqlite3 *db,const char *mac,unsigned sensor_mask);
/* Node's captured per-batch policy overrides the last downlink intention.
 * Already exported immutable messages retain their original snapshot. */
int mqtt_store_batch_policy(sqlite3 *db,const char *mac,uint32_t batch,unsigned sensor_mask);
/* Capture-time scalar bits: temperature, magnetic XYZ, digital SPL, analog SPL.
 * Persist declared unavailable sources so partial captures can still be exported. */
int mqtt_store_batch_scalar_status(sqlite3 *db,const char *mac,uint32_t batch,
                                   unsigned required,unsigned unavailable);
int mqtt_store_note(sqlite3 *db,const char *mac,uint32_t batch,enum MqttBatchEvent event);
int mqtt_store_target(sqlite3 *db,const char *target);
/* Reconcile at most one dirty batch; an internal snapshot/raw are committed
 * together and encoded as Sparkplug Protobuf at publication time. JSON column
 * names are retained for compatibility with existing databases only.
 * 1 queued, 0 nothing ready, 2 quota (source remains dirty), -1 SQL/encoding error. */
int mqtt_store_reconcile(sqlite3 *db,const char *target,const char *gateway,const char *group,
                          sqlite3_int64 max_pending_bytes);
int mqtt_store_next(sqlite3 *db,const char *target,MqttDelivery *out);
/* Sparkplug sends device data only inside an online DBIRTH/DDEATH interval.
 * Offline device backlogs remain durable without blocking other devices. */
int mqtt_store_next_online(sqlite3 *db,const char *target,MqttDelivery *out);
void mqtt_delivery_free(MqttDelivery *item);
/* Mark the exact row/part as written to the network socket (not a broker ACK).
 * 1 whole batch sent, 0 one part sent, -1 error. */
int mqtt_store_ack(sqlite3 *db,sqlite3_int64 id,int waveform);
int mqtt_store_pending(sqlite3 *db,const char *target);
typedef struct {
    sqlite3_int64 pending_batches,pending_bytes,other_pending_batches,last_confirmed_at;
} MqttStoreStats;
int mqtt_store_stats(sqlite3 *db,const char *target,MqttStoreStats *out);
/* All complete local source batches still owed to the saved destination,
 * including disabled/offline/unqueued/quota-blocked batches. Read-only;
 * duplicate revisions count once, pending bytes include their remaining data. */
int mqtt_store_backlog_stats(sqlite3 *db,const char *target,const char *gateway,const char *group,MqttStoreStats *out);
#endif
