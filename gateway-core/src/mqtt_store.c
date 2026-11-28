#include "mqtt_store.h"
#include "cJSON.h"
#include "vibration_wire.h"
#include "scalar_delivery.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int execute(sqlite3 *db,const char *sql) {return sqlite3_exec(db,sql,NULL,NULL,NULL)==SQLITE_OK?0:-1;}
static void bind_identity(sqlite3_stmt *s,const char *mac,uint32_t batch) {
    sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,2,batch);
}
static int migrate_scalar_status(sqlite3 *db) {
    sqlite3_stmt *s=NULL;int found=0,rc;
    if(sqlite3_prepare_v2(db,"PRAGMA table_info(mqtt_batch_state);",-1,&s,NULL)!=SQLITE_OK)return -1;
    while((rc=sqlite3_step(s))==SQLITE_ROW)
        if(!strcmp((const char *)sqlite3_column_text(s,1),"scalar_unavailable"))found=1;
    sqlite3_finalize(s);
    if(rc!=SQLITE_DONE)return -1;
    return found?0:execute(db,"ALTER TABLE mqtt_batch_state ADD COLUMN scalar_unavailable INTEGER NOT NULL DEFAULT 0;");
}
int mqtt_store_init(sqlite3 *db) {
    if(!db)return -1;
    sqlite3_mutex *mu=sqlite3_db_mutex(db);sqlite3_mutex_enter(mu);
    const char *sql=
        "SAVEPOINT mqtt_schema;"
        "CREATE TABLE IF NOT EXISTS mqtt_node_policy(mac TEXT PRIMARY KEY,sensor_mask INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS mqtt_batch_state(mac TEXT NOT NULL,batch_id INTEGER NOT NULL,"
        "sensor_mask INTEGER,needs_vibration INTEGER NOT NULL DEFAULT 0,raw_started INTEGER NOT NULL DEFAULT 0,"
        "summary_seen INTEGER NOT NULL DEFAULT 0,node_complete INTEGER NOT NULL DEFAULT 0,"
        "dirty INTEGER NOT NULL DEFAULT 1,reason TEXT NOT NULL DEFAULT 'waiting',PRIMARY KEY(mac,batch_id));"
        "CREATE INDEX IF NOT EXISTS mqtt_batch_dirty ON mqtt_batch_state(dirty);"
        "CREATE INDEX IF NOT EXISTS mqtt_batch_reason ON mqtt_batch_state(reason);"
        "CREATE INDEX IF NOT EXISTS mqtt_vibration_identity ON vibration_batches(mac,batch_id);"
        "CREATE TABLE IF NOT EXISTS mqtt_targets(target TEXT PRIMARY KEY);"
        "CREATE TABLE IF NOT EXISTS mqtt_outbox(id INTEGER PRIMARY KEY AUTOINCREMENT,target TEXT NOT NULL,"
        "mac TEXT NOT NULL,batch_id INTEGER NOT NULL,canonical TEXT NOT NULL,json_payload TEXT,raw_payload BLOB,"
        "json_topic TEXT NOT NULL,raw_topic TEXT NOT NULL,json_acked INTEGER NOT NULL DEFAULT 0,"
        "raw_acked INTEGER NOT NULL,created INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER)),acked_at INTEGER);"
        "CREATE INDEX IF NOT EXISTS mqtt_outbox_target_pending ON mqtt_outbox(target,json_acked,raw_acked,id);"
        "CREATE INDEX IF NOT EXISTS mqtt_outbox_batch ON mqtt_outbox(target,mac,batch_id,id);"
        "DROP TRIGGER IF EXISTS mqtt_values_insert;DROP TRIGGER IF EXISTS mqtt_values_update;"
        "DROP TRIGGER IF EXISTS mqtt_vib_insert;DROP TRIGGER IF EXISTS mqtt_vib_update;"
        "CREATE TRIGGER IF NOT EXISTS mqtt_values_insert AFTER INSERT ON collection_values BEGIN "
        "INSERT INTO mqtt_batch_state(mac,batch_id) SELECT NEW.mac,NEW.batch_id WHERE NOT EXISTS(SELECT 1 FROM mqtt_batch_state WHERE mac=NEW.mac AND batch_id=NEW.batch_id);"
        "UPDATE mqtt_batch_state SET dirty=1 WHERE mac=NEW.mac AND batch_id=NEW.batch_id;END;"
        "CREATE TRIGGER IF NOT EXISTS mqtt_values_update AFTER UPDATE ON collection_values BEGIN "
        "INSERT INTO mqtt_batch_state(mac,batch_id) SELECT NEW.mac,NEW.batch_id WHERE NOT EXISTS(SELECT 1 FROM mqtt_batch_state WHERE mac=NEW.mac AND batch_id=NEW.batch_id);"
        "UPDATE mqtt_batch_state SET dirty=1 WHERE mac=NEW.mac AND batch_id=NEW.batch_id;END;"
        "CREATE TRIGGER IF NOT EXISTS mqtt_vib_insert AFTER INSERT ON vibration_batches BEGIN "
        "INSERT INTO mqtt_batch_state(mac,batch_id,needs_vibration) SELECT NEW.mac,NEW.batch_id,1 WHERE NOT EXISTS(SELECT 1 FROM mqtt_batch_state WHERE mac=NEW.mac AND batch_id=NEW.batch_id);"
        "UPDATE mqtt_batch_state SET dirty=1,needs_vibration=1 WHERE mac=NEW.mac AND batch_id=NEW.batch_id;END;"
        "CREATE TRIGGER IF NOT EXISTS mqtt_vib_update AFTER UPDATE ON vibration_batches BEGIN "
        "INSERT INTO mqtt_batch_state(mac,batch_id,needs_vibration) SELECT NEW.mac,NEW.batch_id,1 WHERE NOT EXISTS(SELECT 1 FROM mqtt_batch_state WHERE mac=NEW.mac AND batch_id=NEW.batch_id);"
        "UPDATE mqtt_batch_state SET dirty=1,needs_vibration=1 WHERE mac=NEW.mac AND batch_id=NEW.batch_id;END;"
        "CREATE TRIGGER IF NOT EXISTS mqtt_device_delete AFTER DELETE ON devices BEGIN "
        "DELETE FROM mqtt_batch_state WHERE mac=OLD.mac;DELETE FROM mqtt_node_policy WHERE mac=OLD.mac;END;"
        "INSERT OR IGNORE INTO mqtt_batch_state(mac,batch_id) SELECT mac,batch_id FROM collection_values;"
        "INSERT OR IGNORE INTO mqtt_batch_state(mac,batch_id,needs_vibration) SELECT mac,batch_id,1 FROM vibration_batches;";
    int result=execute(db,sql);
    if(!result)result=migrate_scalar_status(db);
    if(!result)result=execute(db,"RELEASE mqtt_schema;");
    if(result) {execute(db,"ROLLBACK TO mqtt_schema;");execute(db,"RELEASE mqtt_schema;");}
    sqlite3_mutex_leave(mu);return result;
}
int mqtt_store_policy(sqlite3 *db,const char *mac,unsigned mask) {
    if(!db || !mac || mask>7)return -1;
    sqlite3_stmt *s=NULL;
    if(sqlite3_prepare_v2(db,"INSERT INTO mqtt_node_policy(mac,sensor_mask) VALUES(?,?) ON CONFLICT(mac) DO UPDATE SET sensor_mask=excluded.sensor_mask;",-1,&s,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,2,(int)mask);
    int rc=sqlite3_step(s);sqlite3_finalize(s);return rc==SQLITE_DONE?0:-1;
}
static int batch_policy(sqlite3 *db,const char *mac,uint32_t batch,unsigned mask,int unavailable) {
    if(!db || !mac || !batch || mask>7U)return -1;
    const char *sql="INSERT INTO mqtt_batch_state(mac,batch_id,sensor_mask,scalar_unavailable) VALUES(?1,?2,?3,COALESCE(?4,0)) "
        "ON CONFLICT(mac,batch_id) DO UPDATE SET sensor_mask=excluded.sensor_mask,scalar_unavailable=COALESCE(?4,scalar_unavailable),dirty=1 "
        "WHERE NOT EXISTS(SELECT 1 FROM mqtt_outbox WHERE mac=?1 AND batch_id=?2);";
    sqlite3_stmt *s=NULL;
    if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK)return -1;
    bind_identity(s,mac,batch);sqlite3_bind_int(s,3,(int)mask);
    if(unavailable<0)sqlite3_bind_null(s,4);else sqlite3_bind_int(s,4,unavailable);
    int rc=sqlite3_step(s);sqlite3_finalize(s);return rc==SQLITE_DONE?0:-1;
}
int mqtt_store_batch_policy(sqlite3 *db,const char *mac,uint32_t batch,unsigned mask) {
    return batch_policy(db,mac,batch,mask,-1);
}
int mqtt_store_batch_scalar_status(sqlite3 *db,const char *mac,uint32_t batch,unsigned required,unsigned unavailable) {
    if((required&~SCALAR_ALL) || (unavailable&~required))return -1;
    unsigned sensors=1U|((required&4U)?2U:0U)|((required&8U)?4U:0U);
    return batch_policy(db,mac,batch,sensors,(int)unavailable);
}
int mqtt_store_note(sqlite3 *db,const char *mac,uint32_t batch,enum MqttBatchEvent event) {
    if(!db || !mac || !batch || event<1 || event>5)return -1;
    sqlite3_mutex *mu=sqlite3_db_mutex(db);sqlite3_mutex_enter(mu);
    sqlite3_stmt *s=NULL;int result=-1;
    if(execute(db,"SAVEPOINT mqtt_note;"))goto done;
    if(sqlite3_prepare_v2(db,"INSERT OR IGNORE INTO mqtt_batch_state(mac,batch_id) VALUES(?,?);",-1,&s,NULL)!=SQLITE_OK)goto rollback;
    bind_identity(s,mac,batch);int rc=sqlite3_step(s);sqlite3_finalize(s);s=NULL;
    if(rc!=SQLITE_DONE)goto rollback;
    const char *column=event==MQTT_BATCH_RAW?"raw_started":event==MQTT_BATCH_SUMMARY?"summary_seen":
                       event==MQTT_BATCH_COMPLETE?"node_complete":"needs_vibration";
    char sql[768];
    if(event==MQTT_BATCH_CAPTURE) {
        snprintf(sql,sizeof(sql),"UPDATE mqtt_batch_state SET sensor_mask=COALESCE(sensor_mask,(SELECT sensor_mask FROM mqtt_node_policy WHERE mac=?1)),dirty=1 WHERE mac=?1 AND batch_id=?2;");
    } else {
        snprintf(sql,sizeof(sql),"UPDATE mqtt_batch_state SET %s=1,sensor_mask=COALESCE(sensor_mask,(SELECT sensor_mask FROM mqtt_node_policy WHERE mac=?1)),dirty=1 WHERE mac=?1 AND batch_id=?2 AND (%s=0 OR sensor_mask IS NULL);",column,column);
    }
    if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK)goto rollback;
    bind_identity(s,mac,batch);rc=sqlite3_step(s);sqlite3_finalize(s);s=NULL;
    if(rc!=SQLITE_DONE || execute(db,"RELEASE mqtt_note;"))goto rollback;
    result=0;goto done;
rollback:
    sqlite3_finalize(s);execute(db,"ROLLBACK TO mqtt_note;");execute(db,"RELEASE mqtt_note;");
done:
    sqlite3_mutex_leave(mu);return result;
}
int mqtt_store_target(sqlite3 *db,const char *target) {
    sqlite3_mutex *mu=sqlite3_db_mutex(db);sqlite3_mutex_enter(mu);
    sqlite3_stmt *s=NULL;int result=-1;
    if(execute(db,"SAVEPOINT mqtt_target;"))goto done;
    if(sqlite3_prepare_v2(db,"INSERT OR IGNORE INTO mqtt_targets(target) VALUES(?);",-1,&s,NULL)!=SQLITE_OK)goto rollback;
    sqlite3_bind_text(s,1,target,-1,SQLITE_TRANSIENT);int rc=sqlite3_step(s);
    sqlite3_finalize(s);s=NULL;if(rc!=SQLITE_DONE)goto rollback;
    /* Revisit sources on every startup, not just first use of a destination:
     * a source may have changed while a different destination was active. */
    if(execute(db,"UPDATE mqtt_batch_state SET dirty=1;"))goto rollback;
    if(execute(db,"RELEASE mqtt_target;"))goto rollback;
    result=0;goto done;
rollback:
    sqlite3_finalize(s);execute(db,"ROLLBACK TO mqtt_target;");execute(db,"RELEASE mqtt_target;");
done:
    sqlite3_mutex_leave(mu);return result;
}
static cJSON *number_or_null(cJSON *object,const char *name,sqlite3_stmt *s,int column,int unavailable) {
    cJSON *v;
    if(unavailable || sqlite3_column_type(s,column)==SQLITE_NULL)v=cJSON_CreateNull();
    else {
        double value=sqlite3_column_double(s,column),scale=column>=8 && column<=10?10.0:100.0;
        /* Remove float-storage noise at the actual sensor wire precision. */
        if(value>-1e12 && value<1e12)value=(double)(sqlite3_int64)(value*scale+(value<0?-0.5:0.5))/scale;
        v=cJSON_CreateNumber(value);
    }
    cJSON_AddItemToObject(object,name,v);return v;
}
static int same_snapshot(const char *previous,const char *current) {
    if(!strcmp(previous,current))return 1;
    /* The retired topic hint is not measurement content. Updating to the
     * shared DDATA topic must not manufacture revisions of completed batches. */
    cJSON *a=cJSON_Parse(previous),*b=cJSON_Parse(current);
    if(a)cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(a,"waveform"),"topic");
    if(b)cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(b,"waveform"),"topic");
    int equal=a && b && cJSON_Compare(a,b,1);cJSON_Delete(a);cJSON_Delete(b);return equal;
}
/* A preview uses the same readiness/snapshot rules as publication, but never
 * changes dirty flags, outbox rows, quotas or delivery state. */
static int store_reconcile(sqlite3 *db,const char *target,const char *gateway,const char *group,
    sqlite3_int64 quota,const char *preview_mac,uint32_t preview_batch,MqttStoreStats *preview) {
    sqlite3_mutex *mu=sqlite3_db_mutex(db);sqlite3_mutex_enter(mu);
    sqlite3_stmt *s=NULL,*w=NULL;int result=-1;
    cJSON *root=NULL;char *canonical=NULL,*json=NULL;const char *reason="waiting_for_fields";
    char mac[24]={0},compact[17]={0},json_topic[512],raw_topic[512];uint32_t batch=0;
    if(!preview && execute(db,"SAVEPOINT mqtt_reconcile;"))goto done;
    const char *columns=
        "SELECT b.mac,b.batch_id,b.sensor_mask,b.needs_vibration,b.raw_started,b.summary_seen,b.node_complete,"
        "c.temperature,c.mag_x,c.mag_y,c.mag_z,c.digital_db,c.analog_db,c.timestamp,"
        "v.sample_rate_hz,v.sample_count,v.features,v.samples,v.timestamp,b.scalar_unavailable "
        "FROM mqtt_batch_state b JOIN devices d ON d.mac=b.mac "
        "LEFT JOIN collection_values c ON c.mac=b.mac AND c.batch_id=b.batch_id "
        "LEFT JOIN vibration_batches v ON v.mac=b.mac AND v.batch_id=b.batch_id ";
    char select[1200];
    int selected=snprintf(select,sizeof(select),"%s%s",columns,preview?
        "WHERE b.mac=?1 AND b.batch_id=?2;":"WHERE b.dirty=1 ORDER BY b.rowid LIMIT 1;");
    if(selected<0 || selected>=(int)sizeof(select))goto rollback;
    if(sqlite3_prepare_v2(db,select,-1,&s,NULL)!=SQLITE_OK)goto rollback;
    if(preview)bind_identity(s,preview_mac,preview_batch);
    int rc=sqlite3_step(s);if(rc==SQLITE_DONE){result=0;goto commit;}
    if(rc!=SQLITE_ROW)goto rollback;
    snprintf(mac,sizeof(mac),"%s",sqlite3_column_text(s,0));batch=(uint32_t)sqlite3_column_int64(s,1);
    unsigned pos=0;for(unsigned i=0;mac[i] && pos<16;i++)if(mac[i]!=':')compact[pos++]=mac[i];
    if(pos!=16){reason="invalid_mac";goto defer;}
    /* Unknown old policy is conservative: require all scalar channels. */
    unsigned sensors=sqlite3_column_type(s,2)==SQLITE_NULL?7U:(unsigned)sqlite3_column_int(s,2);
    unsigned unavailable=(unsigned)sqlite3_column_int(s,19)&SCALAR_ALL;
    unsigned expected=scalar_expected_mask((uint8_t)sensors)&~unavailable;
    if((expected&1U) && sqlite3_column_type(s,7)==SQLITE_NULL)goto defer;
    if(expected&2U)for(int i=8;i<=10;i++)if(sqlite3_column_type(s,i)==SQLITE_NULL)goto defer;
    if((expected&4U) && sqlite3_column_type(s,11)==SQLITE_NULL)goto defer;
    if((expected&8U) && sqlite3_column_type(s,12)==SQLITE_NULL)goto defer;
    int needs_vibration=(sensors&1) || sqlite3_column_int(s,3) || sqlite3_column_type(s,15)!=SQLITE_NULL;
    uint32_t rate=(uint32_t)sqlite3_column_int(s,14),count=(uint32_t)sqlite3_column_int(s,15);
    int32_t features[3][5];memset(features,0,sizeof(features));
    const void *raw=sqlite3_column_blob(s,17);int raw_bytes=sqlite3_column_bytes(s,17);
    if(needs_vibration) {
        reason="waiting_for_features";
        if(!count || count>V2_MAX_SAMPLES || !rate || sqlite3_column_bytes(s,16)!=(int)sizeof(features))goto defer;
        memcpy(features,sqlite3_column_blob(s,16),sizeof(features));
        if(v2_features_pending(features))goto defer;
        for(unsigned a=0;a<3;a++)for(unsigned f=0;f<5;f++)if(features[a][f]==INT32_MIN)goto defer;
        reason="waiting_for_waveform";
        if(raw_bytes && raw_bytes!=(int)(count*6U))goto defer;
        if(!raw_bytes && sqlite3_column_int(s,4))goto defer;
        if(!raw_bytes && !sqlite3_column_int(s,5) && !sqlite3_column_int(s,6)) {
            reason="legacy_completion_unknown";goto defer;
        }
    } else if(!sqlite3_column_int(s,6)) {reason="waiting_for_completion";goto defer;}
    root=cJSON_CreateObject();if(!root)goto rollback;
    cJSON_AddNumberToObject(root,"schema_version",1);
    cJSON_AddStringToObject(root,"gateway_id",gateway);cJSON_AddStringToObject(root,"mac",mac);
    cJSON_AddNumberToObject(root,"batch_id",batch);cJSON_AddBoolToObject(root,"collection_complete",1);
    /* The database timestamp is receipt time, not a synchronized node clock. */
    cJSON_AddNumberToObject(root,"received_at",sqlite3_column_int64(s,sqlite3_column_type(s,13)==SQLITE_NULL?18:13));
    cJSON_AddNullToObject(root,"collected_at");
    cJSON *values=cJSON_AddObjectToObject(root,"measurements");
    number_or_null(values,"temperature_c",s,7,unavailable&1U);
    cJSON *mag=cJSON_AddObjectToObject(values,"magnetic_ut");
    number_or_null(mag,"x",s,8,unavailable&2U);number_or_null(mag,"y",s,9,unavailable&2U);number_or_null(mag,"z",s,10,unavailable&2U);
    number_or_null(values,"digital_mic_db_spl",s,11,unavailable&4U);number_or_null(values,"ultrasonic_mic_db_spl",s,12,unavailable&8U);
    cJSON *capture=NULL,*absent=NULL;unsigned absent_count=0;
    if(unavailable) {
        capture=cJSON_AddObjectToObject(root,"capture_status");
        if(!capture || !cJSON_AddBoolToObject(capture,"complete",0))goto rollback;
        absent=cJSON_AddArrayToObject(capture,"unavailable_measurements");
        if(!absent)goto rollback;
        const char *names[]={"temperature_c","magnetic_ut","digital_mic_db_spl","ultrasonic_mic_db_spl"};
        for(unsigned i=0;i<4;i++)if(unavailable&(1U<<i)) {
            cJSON *name=cJSON_CreateString(names[i]);
            if(!name)goto rollback;
            if(!cJSON_AddItemToArray(absent,name)){cJSON_Delete(name);goto rollback;}
            absent_count++;
        }
    }
    cJSON *vib=cJSON_AddObjectToObject(root,"vibration");cJSON_AddBoolToObject(vib,"available",needs_vibration);
    cJSON_AddNumberToObject(vib,"sample_rate_hz",needs_vibration?rate:0);
    cJSON_AddNumberToObject(vib,"sample_count",needs_vibration?count:0);
    const char *axes[]={"x","y","z"},*names[]={"rms_g","peak_g","peak_to_peak_g","kurtosis","dominant_frequency_hz"};
    cJSON *feature_json=cJSON_AddObjectToObject(vib,"features");
    if(needs_vibration)for(unsigned a=0;a<3;a++) {
        cJSON *axis=cJSON_AddObjectToObject(feature_json,axes[a]);
        for(unsigned f=0;f<5;f++)cJSON_AddNumberToObject(axis,names[f],features[a][f]/1000.0);
    }
    /* Both DDATA parts share the Sparkplug device topic. Legacy SQL column
     * names remain readable; no private JSON/binary topics are generated. */
    if(snprintf(json_topic,sizeof(json_topic),"spBv1.0/%s/DDATA/%s/%s",group,gateway,compact)>=(int)sizeof(json_topic))goto rollback;
    snprintf(raw_topic,sizeof(raw_topic),"%s",json_topic);
    cJSON *wave=cJSON_AddObjectToObject(root,"waveform");
    cJSON_AddBoolToObject(wave,"available",raw_bytes>0);
    cJSON_AddStringToObject(wave,"status",raw_bytes?"present":"not_in_batch");
    cJSON_AddStringToObject(wave,"encoding","xyz-int16-le");cJSON_AddNumberToObject(wave,"scale_g_per_lsb",0.000488);
    cJSON_AddNumberToObject(wave,"bytes",raw_bytes);
    char crc[9];snprintf(crc,sizeof(crc),"%08X",v2_crc(raw,(size_t)raw_bytes));
    if(raw_bytes)cJSON_AddStringToObject(wave,"crc32",crc);else cJSON_AddNullToObject(wave,"crc32");
    if(raw_bytes)cJSON_AddStringToObject(wave,"topic",raw_topic);else cJSON_AddNullToObject(wave,"topic");
    /* cJSON allocation failure must never turn a partial object into a
     * durable message claiming to contain a complete collection. */
    if(cJSON_GetArraySize(root)!=(unavailable?11:10) || cJSON_GetArraySize(values)!=4 || cJSON_GetArraySize(mag)!=3 ||
       cJSON_GetArraySize(vib)!=4 || cJSON_GetArraySize(wave)!=7 ||
       cJSON_GetArraySize(feature_json)!=(needs_vibration?3:0))goto rollback;
    if(unavailable && (cJSON_GetArraySize(capture)!=2 || cJSON_GetArraySize(absent)!=(int)absent_count))goto rollback;
    if(needs_vibration)for(unsigned a=0;a<3;a++)
        if(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(feature_json,axes[a]))!=5)goto rollback;
    canonical=cJSON_PrintUnformatted(root);if(!canonical)goto rollback;
    if(sqlite3_prepare_v2(db,"SELECT canonical,EXISTS(SELECT 1 FROM mqtt_outbox p WHERE p.target=?1 AND p.mac=?2 AND p.batch_id=?3 AND (p.json_acked=0 OR p.raw_acked=0)) FROM mqtt_outbox WHERE target=?1 AND mac=?2 AND batch_id=?3 ORDER BY id DESC LIMIT 1;",-1,&w,NULL)!=SQLITE_OK)goto rollback;
    sqlite3_bind_text(w,1,target,-1,SQLITE_TRANSIENT);sqlite3_bind_text(w,2,mac,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(w,3,batch);
    rc=sqlite3_step(w);int identical=rc==SQLITE_ROW && same_snapshot((const char *)sqlite3_column_text(w,0),canonical);
    int already_pending=rc==SQLITE_ROW && sqlite3_column_int(w,1);
    sqlite3_finalize(w);w=NULL;if(rc!=SQLITE_ROW && rc!=SQLITE_DONE)goto rollback;
    if(identical){reason="queued";goto defer;}
    if(preview) {
        if(!already_pending)preview->pending_batches++;
        preview->pending_bytes+=(sqlite3_int64)strlen(canonical)+raw_bytes;
        result=0;goto done;
    }
    /* Legacy JSON destinations are preserved as archives, never sent. Their
     * bytes must not prevent new Sparkplug destinations from making progress. */
    if(sqlite3_prepare_v2(db,"SELECT COALESCE(sum(COALESCE(length(json_payload),0)+COALESCE(length(raw_payload),0)),0) FROM mqtt_outbox WHERE (json_acked=0 OR raw_acked=0) AND target GLOB '*|sparkplug_b:*';",-1,&w,NULL)!=SQLITE_OK)goto rollback;
    rc=sqlite3_step(w);if(rc!=SQLITE_ROW)goto rollback;
    sqlite3_int64 pending=sqlite3_column_int64(w,0);sqlite3_finalize(w);w=NULL;
    if(quota>0 && pending+(sqlite3_int64)strlen(canonical)+raw_bytes+256>quota) {result=2;goto commit;}
    if(sqlite3_prepare_v2(db,"INSERT INTO mqtt_outbox(target,mac,batch_id,canonical,raw_payload,json_topic,raw_topic,raw_acked) VALUES(?,?,?,?,?,?,?,?);",-1,&w,NULL)!=SQLITE_OK)goto rollback;
    sqlite3_bind_text(w,1,target,-1,SQLITE_TRANSIENT);sqlite3_bind_text(w,2,mac,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(w,3,batch);
    sqlite3_bind_text(w,4,canonical,-1,SQLITE_TRANSIENT);
    if(raw_bytes)sqlite3_bind_blob(w,5,raw,raw_bytes,SQLITE_TRANSIENT);else sqlite3_bind_null(w,5);
    sqlite3_bind_text(w,6,json_topic,-1,SQLITE_TRANSIENT);sqlite3_bind_text(w,7,raw_topic,-1,SQLITE_TRANSIENT);sqlite3_bind_int(w,8,!raw_bytes);
    rc=sqlite3_step(w);sqlite3_finalize(w);w=NULL;if(rc!=SQLITE_DONE)goto rollback;
    sqlite3_int64 id=sqlite3_last_insert_rowid(db);char message_id[192];
    snprintf(message_id,sizeof(message_id),"%s/%s/%u/%lld",gateway,compact,batch,(long long)id);
    cJSON_AddStringToObject(root,"message_id",message_id);cJSON_AddNumberToObject(root,"revision",(double)id);
    /* Batch/MessageId and Batch/Revision associate the result and raw Bytes
     * metrics on the same DDATA topic. */
    if(cJSON_GetArraySize(root)!=(unavailable?13:12))goto rollback;
    json=cJSON_PrintUnformatted(root);if(!json)goto rollback;
    if(sqlite3_prepare_v2(db,"UPDATE mqtt_outbox SET json_payload=?,raw_topic=? WHERE id=?;",-1,&w,NULL)!=SQLITE_OK)goto rollback;
    sqlite3_bind_text(w,1,json,-1,SQLITE_TRANSIENT);sqlite3_bind_text(w,2,raw_topic,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(w,3,id);rc=sqlite3_step(w);sqlite3_finalize(w);w=NULL;
    if(rc!=SQLITE_DONE)goto rollback;
    result=1;reason="queued";
defer:
    if(preview){result=0;goto done;}
    if(sqlite3_prepare_v2(db,"UPDATE mqtt_batch_state SET dirty=0,reason=? WHERE mac=? AND batch_id=?;",-1,&w,NULL)!=SQLITE_OK)goto rollback;
    sqlite3_bind_text(w,1,reason,-1,SQLITE_TRANSIENT);sqlite3_bind_text(w,2,mac,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(w,3,batch);
    rc=sqlite3_step(w);sqlite3_finalize(w);w=NULL;if(rc!=SQLITE_DONE)goto rollback;
    if(result<0)result=0;
commit:
    sqlite3_finalize(s);s=NULL;
    if(!preview && execute(db,"RELEASE mqtt_reconcile;"))goto rollback;
    goto done;
rollback:
    sqlite3_finalize(w);sqlite3_finalize(s);s=NULL;
    if(!preview){execute(db,"ROLLBACK TO mqtt_reconcile;");execute(db,"RELEASE mqtt_reconcile;");}
    result=-1;
done:
    if(preview)sqlite3_finalize(s);
    cJSON_Delete(root);free(canonical);free(json);sqlite3_mutex_leave(mu);return result;
}
int mqtt_store_reconcile(sqlite3 *db,const char *target,const char *gateway,const char *group,sqlite3_int64 quota) {
    return store_reconcile(db,target,gateway,group,quota,NULL,0,NULL);
}
static int store_next(sqlite3 *db,const char *target,MqttDelivery *out,int online) {
    memset(out,0,sizeof(*out));sqlite3_stmt *s=NULL;
    const char *sql="SELECT id,mac,batch_id,json_acked,json_topic,raw_topic,json_payload,raw_payload FROM mqtt_outbox o WHERE target=?1 AND (json_acked=0 OR raw_acked=0) AND (?2=0 OR EXISTS(SELECT 1 FROM devices d WHERE d.mac=o.mac AND d.status='online')) ORDER BY id LIMIT 1;";
    if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(s,1,target,-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,2,online);int rc=sqlite3_step(s),result=-1;
    if(rc==SQLITE_DONE){result=0;goto done;}if(rc!=SQLITE_ROW)goto done;
    out->id=sqlite3_column_int64(s,0);out->batch=(uint32_t)sqlite3_column_int64(s,2);out->waveform=sqlite3_column_int(s,3)!=0;
    snprintf(out->mac,sizeof(out->mac),"%s",sqlite3_column_text(s,1));
    snprintf(out->topic,sizeof(out->topic),"%s",sqlite3_column_text(s,out->waveform?5:4));
    int col=out->waveform?7:6,bytes=sqlite3_column_bytes(s,col);
    if(bytes<=0)goto done;
    out->payload=malloc((size_t)bytes+1);if(!out->payload)goto done;
    memcpy(out->payload,sqlite3_column_blob(s,col),(size_t)bytes);((char *)out->payload)[bytes]=0;
    out->length=(size_t)bytes;result=1;
done:
    sqlite3_finalize(s);return result;
}
int mqtt_store_next(sqlite3 *db,const char *target,MqttDelivery *out){return store_next(db,target,out,0);}
int mqtt_store_next_online(sqlite3 *db,const char *target,MqttDelivery *out){return store_next(db,target,out,1);}
void mqtt_delivery_free(MqttDelivery *item) {free(item->payload);memset(item,0,sizeof(*item));}
int mqtt_store_ack(sqlite3 *db,sqlite3_int64 id,int waveform) {
    sqlite3_mutex *mu=sqlite3_db_mutex(db);sqlite3_mutex_enter(mu);
    sqlite3_stmt *s=NULL;int result=-1;
    if(execute(db,"SAVEPOINT mqtt_ack;"))goto done;
    const char *sql=waveform?"UPDATE mqtt_outbox SET raw_acked=1 WHERE id=? AND json_acked=1;":"UPDATE mqtt_outbox SET json_acked=1 WHERE id=?;";
    if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK)goto rollback;
    sqlite3_bind_int64(s,1,id);int rc=sqlite3_step(s),changed=sqlite3_changes(db);sqlite3_finalize(s);s=NULL;
    if(rc!=SQLITE_DONE || !changed)goto rollback;
    if(sqlite3_prepare_v2(db,"UPDATE mqtt_outbox SET acked_at=CAST(strftime('%s','now') AS INTEGER),json_payload=NULL,raw_payload=NULL WHERE id=? AND json_acked=1 AND raw_acked=1;",-1,&s,NULL)!=SQLITE_OK)goto rollback;
    sqlite3_bind_int64(s,1,id);rc=sqlite3_step(s);result=sqlite3_changes(db)>0;sqlite3_finalize(s);s=NULL;
    if(rc!=SQLITE_DONE || execute(db,"RELEASE mqtt_ack;"))goto rollback;
    goto done;
rollback:
    sqlite3_finalize(s);execute(db,"ROLLBACK TO mqtt_ack;");execute(db,"RELEASE mqtt_ack;");result=-1;
done:
    sqlite3_mutex_leave(mu);return result;
}
int mqtt_store_pending(sqlite3 *db,const char *target) {
    sqlite3_stmt *s=NULL;int result=-1;
    if(sqlite3_prepare_v2(db,"SELECT count(*) FROM mqtt_outbox WHERE target=? AND (json_acked=0 OR raw_acked=0);",-1,&s,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(s,1,target,-1,SQLITE_TRANSIENT);if(sqlite3_step(s)==SQLITE_ROW)result=sqlite3_column_int(s,0);
    sqlite3_finalize(s);return result;
}
int mqtt_store_stats(sqlite3 *db,const char *target,MqttStoreStats *out) {
    memset(out,0,sizeof(*out));
    if(!db || !target)return -1;
    sqlite3_stmt *s=NULL;
    const char *sql="SELECT "
        "COALESCE(sum(CASE WHEN target=?1 AND (json_acked=0 OR raw_acked=0) THEN 1 ELSE 0 END),0),"
        "COALESCE(sum(CASE WHEN target=?1 THEN (CASE WHEN json_acked=0 THEN COALESCE(length(json_payload),0) ELSE 0 END)+"
        "(CASE WHEN raw_acked=0 THEN COALESCE(length(raw_payload),0) ELSE 0 END) ELSE 0 END),0),"
        "COALESCE(sum(CASE WHEN target<>?1 AND (json_acked=0 OR raw_acked=0) THEN 1 ELSE 0 END),0),"
        "COALESCE(max(CASE WHEN target=?1 THEN acked_at ELSE NULL END),0) FROM mqtt_outbox;";
    if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(s,1,target,-1,SQLITE_TRANSIENT);int rc=sqlite3_step(s);
    if(rc==SQLITE_ROW) {
        out->pending_batches=sqlite3_column_int64(s,0);out->pending_bytes=sqlite3_column_int64(s,1);
        out->other_pending_batches=sqlite3_column_int64(s,2);out->last_confirmed_at=sqlite3_column_int64(s,3);
    }
    sqlite3_finalize(s);return rc==SQLITE_ROW?0:-1;
}
int mqtt_store_backlog_stats(sqlite3 *db,const char *target,const char *gateway,const char *group,MqttStoreStats *out) {
    if(!out)return -1;
    memset(out,0,sizeof(*out));
    if(!db || !target || !gateway || !group)return -1;
    sqlite3_mutex *mu=sqlite3_db_mutex(db);sqlite3_mutex_enter(mu);
    sqlite3_stmt *s=NULL;MqttStoreStats stats={0};int result=-1,rc;
    if(execute(db,"SAVEPOINT mqtt_backlog;"))goto done;
    if(mqtt_store_stats(db,target,&stats))goto rollback;
    /* Count source batches, not transport revisions. Size is the remaining
     * immutable measurement snapshot plus raw bytes, excluding MQTT framing
     * and dynamic message/revision identifiers, both queued and unqueued. */
    const char *sql="SELECT (SELECT count(*) FROM (SELECT mac,batch_id FROM mqtt_outbox "
        "WHERE target=?1 AND (json_acked=0 OR raw_acked=0) GROUP BY mac,batch_id)),"
        "COALESCE(sum((CASE WHEN json_acked=0 THEN length(CAST(canonical AS BLOB)) ELSE 0 END)+"
        "(CASE WHEN raw_acked=0 THEN COALESCE(length(raw_payload),0) ELSE 0 END)),0) "
        "FROM mqtt_outbox WHERE target=?1 AND (json_acked=0 OR raw_acked=0);";
    if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK)goto rollback;
    sqlite3_bind_text(s,1,target,-1,SQLITE_TRANSIENT);
    if(sqlite3_step(s)!=SQLITE_ROW)goto rollback;
    stats.pending_batches=sqlite3_column_int64(s,0);stats.pending_bytes=sqlite3_column_int64(s,1);
    sqlite3_finalize(s);s=NULL;
    if(sqlite3_prepare_v2(db,"SELECT b.mac,b.batch_id FROM mqtt_batch_state b JOIN devices d ON d.mac=b.mac;",-1,&s,NULL)!=SQLITE_OK)goto rollback;
    while((rc=sqlite3_step(s))==SQLITE_ROW) {
        const char *mac=(const char *)sqlite3_column_text(s,0);
        uint32_t batch=(uint32_t)sqlite3_column_int64(s,1);
        if(store_reconcile(db,target,gateway,group,0,mac,batch,&stats)<0)goto rollback;
    }
    if(rc!=SQLITE_DONE)goto rollback;
    sqlite3_finalize(s);s=NULL;
    if(execute(db,"RELEASE mqtt_backlog;"))goto rollback;
    *out=stats;result=0;goto done;
rollback:
    sqlite3_finalize(s);s=NULL;
    execute(db,"ROLLBACK TO mqtt_backlog;");execute(db,"RELEASE mqtt_backlog;");
done:
    sqlite3_finalize(s);sqlite3_mutex_leave(mu);return result;
}
