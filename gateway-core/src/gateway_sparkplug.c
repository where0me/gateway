#include "gateway_sparkplug.h"
#include "sparkplug_b.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct SpbDevice {
    char mac[24]; int born,seen;
    struct SpbDevice *next;
} SpbDevice;
struct GatewaySparkplug {
    unsigned bdseq,sequence,phase;
    uint16_t subscription;
    int rebirth,sending,pending_count;
    uint64_t deadline,scan_at,rebirth_at,bad_command_at;
    SpbDevice *devices;
    SpbBuffer will;
    char will_topic[256],command_topic[256];
};
static uint64_t utc_ms(void) {
    struct timespec ts;
    if(timespec_get(&ts,TIME_UTC)==TIME_UTC)return (uint64_t)ts.tv_sec*1000U+(unsigned long)ts.tv_nsec/1000000U;
    return (uint64_t)time(NULL)*1000U;
}
static void fail(GatewayMqtt *m,const char *code,const char *message) {
    snprintf(m->error_code,sizeof(m->error_code),"%s",code);
    snprintf(m->error_message,sizeof(m->error_message),"%s",message);m->error_at=(int64_t)time(NULL);
    m->ready=0;
    if(m->connection)m->connection->is_closing=1;
}
void gateway_sparkplug_clear(GatewayMqtt *m) {
    struct GatewaySparkplug *s=m->sparkplug;if(!s)return;
    while(s->devices){SpbDevice *d=s->devices;s->devices=d->next;free(d);}
    spb_free(&s->will);free(s);m->sparkplug=NULL;
}
static void device_id(const char *mac,char id[17]) {
    unsigned n=0;for(unsigned i=0;mac[i] && n<16;i++)if(mac[i]!=':')id[n++]=mac[i];id[n]=0;
}
static int topic(GatewayMqtt *m,char out[256],const char *type,const char *mac) {
    char id[17];device_id(mac?mac:"",id);
    int n=snprintf(out,256,"spBv1.0/%s/%s/%s%s%s",m->config.group_id,type,m->config.gateway_id,mac?"/":"",id);
    return n>0 && n<256;
}
static int next_bdseq(GatewayMqtt *m,unsigned *value) {
    if(sqlite3_exec(m->db,"CREATE TABLE IF NOT EXISTS sparkplug_sessions(identity TEXT PRIMARY KEY,bdseq INTEGER NOT NULL);",NULL,NULL,NULL)!=SQLITE_OK)return 0;
    sqlite3_stmt *stmt=NULL;char identity[512];
    int n=snprintf(identity,sizeof(identity),"%s|%s|%s",m->config.url,m->config.group_id,m->config.gateway_id);
    if(n<0 || n>=(int)sizeof(identity))return 0;
    sqlite3_mutex *mu=sqlite3_db_mutex(m->db);sqlite3_mutex_enter(mu);int ok=0;
    if(sqlite3_exec(m->db,"SAVEPOINT sparkplug_bdseq;",NULL,NULL,NULL)!=SQLITE_OK)goto done;
    const char *sql="INSERT INTO sparkplug_sessions(identity,bdseq) VALUES(?,0) ON CONFLICT(identity) DO UPDATE SET bdseq=(bdseq+1)%256;";
    if(sqlite3_prepare_v2(m->db,sql,-1,&stmt,NULL)!=SQLITE_OK)goto rollback;
    sqlite3_bind_text(stmt,1,identity,-1,SQLITE_TRANSIENT);
    int rc=sqlite3_step(stmt);sqlite3_finalize(stmt);stmt=NULL;if(rc!=SQLITE_DONE)goto rollback;
    if(sqlite3_prepare_v2(m->db,"SELECT bdseq FROM sparkplug_sessions WHERE identity=?;",-1,&stmt,NULL)!=SQLITE_OK)goto rollback;
    sqlite3_bind_text(stmt,1,identity,-1,SQLITE_TRANSIENT);
    if(sqlite3_step(stmt)!=SQLITE_ROW)goto rollback;
    *value=(unsigned)sqlite3_column_int(stmt,0);sqlite3_finalize(stmt);stmt=NULL;
    if(sqlite3_exec(m->db,"RELEASE sparkplug_bdseq;",NULL,NULL,NULL)==SQLITE_OK){ok=1;goto done;}
rollback:
    sqlite3_finalize(stmt);stmt=NULL;sqlite3_exec(m->db,"ROLLBACK TO sparkplug_bdseq;RELEASE sparkplug_bdseq;",NULL,NULL,NULL);
done:
    sqlite3_mutex_leave(mu);return ok;
}
int gateway_sparkplug_options(GatewayMqtt *m,struct mg_mqtt_opts *opts) {
    gateway_sparkplug_clear(m);
    struct GatewaySparkplug *s=calloc(1,sizeof(*s));if(!s)return 0;m->sparkplug=s;
    if(!next_bdseq(m,&s->bdseq) || !topic(m,s->will_topic,"NDEATH",NULL) ||
       !topic(m,s->command_topic,"NCMD",NULL))return 0;
    uint64_t now=utc_ms();spb_begin(&s->will,now,-1);
    SpbMetric metric={.name="bdSeq",.datatype=SPB_INT64,.integer=s->bdseq,.timestamp=now};
    if(!spb_add(&s->will,&metric,1))return 0;
    opts->topic=mg_str(s->will_topic);opts->message=mg_str_n((char *)s->will.data,s->will.length);
    opts->qos=1;opts->retain=false;opts->clean=true;s->pending_count=-1;
    return 1;
}
void gateway_sparkplug_open(GatewayMqtt *m,uint64_t now) {
    struct GatewaySparkplug *s=m->sparkplug;if(!s)return;
    struct mg_mqtt_opts sub={0};sub.topic=mg_str(s->command_topic);sub.qos=1;
    mg_mqtt_sub(m->connection,&sub);s->subscription=m->mgr->mqtt_id;
    s->phase=1;s->deadline=now+m->config.connect_timeout_ms;
}
void gateway_sparkplug_event(GatewayMqtt *m,int event,const struct mg_mqtt_message *message) {
    struct GatewaySparkplug *s=m->sparkplug;if(!s)return;
    if(event==MG_EV_MQTT_CMD && message->cmd==MQTT_CMD_SUBACK && s->phase==1 && message->id==s->subscription) {
        /* This client is MQTT 3.1.1 and issued exactly one QoS 1 subscription. */
        if(message->dgram.len!=5 || (uint8_t)message->dgram.buf[4]!=1) {
            fail(m,"subscription_denied","Sparkplug 命令主题订阅未获 QoS 1 授权");return;
        }
        s->phase=2;
    }
    if(event==MG_EV_MQTT_MSG && mg_strcmp(message->topic,mg_str(s->command_topic))==0) {
        if(!message->dgram.len || ((uint8_t)message->dgram.buf[0]&1U))return; /* no retained commands */
        int request=spb_rebirth_request(message->data.buf,message->data.len);
        if(request==1)s->rebirth=1;
        else if(request<0 && mg_millis()-s->bad_command_at>=10000U) {
            s->bad_command_at=mg_millis();fprintf(stderr,"[Sparkplug] 忽略格式无效的重发设备信息请求\n");
        }
    }
}
static int publish(GatewayMqtt *m,const char *type,const char *mac,SpbBuffer *b) {
    char t[256];struct mg_connection *c=m->connection;
    if(b->failed || !topic(m,t,type,mac) || !c || c->is_closing){spb_free(b);return 0;}
    struct mg_mqtt_opts opts={0};opts.topic=mg_str(t);opts.message=mg_str_n((char *)b->data,b->length);
    /* Sparkplug NBIRTH/DBIRTH/NDATA/DDATA/DDEATH use QoS 0, retain=false. */
    size_t before=c->send.len;mg_mqtt_pub(c,&opts);spb_free(b);
    if(c->is_closing || c->send.len<=before)return 0;
    m->sparkplug->sequence=(m->sparkplug->sequence+1U)&255U;return 1;
}
static int node_birth(GatewayMqtt *m) {
    struct GatewaySparkplug *s=m->sparkplug;uint64_t now=utc_ms();SpbBuffer b;
    int pending=mqtt_store_pending(m->db,m->target);if(pending<0)return 0;
    s->sequence=0;spb_begin(&b,now,0);
    SpbMetric metrics[]={
        {.name="Node Control/Rebirth",.alias=1,.datatype=SPB_BOOLEAN,.timestamp=now},
        {.name="bdSeq",.alias=2,.datatype=SPB_INT64,.timestamp=now,.integer=s->bdseq},
        {.name="Properties/GatewayId",.alias=3,.datatype=SPB_STRING,.timestamp=now,.data=m->config.gateway_id,.length=strlen(m->config.gateway_id)},
        {.name="Upload/PendingBatches",.alias=4,.datatype=SPB_UINT32,.timestamp=now,.integer=(unsigned)pending}
    };
    for(unsigned i=0;i<sizeof(metrics)/sizeof(metrics[0]);i++)spb_add(&b,&metrics[i],1);
    if(!publish(m,"NBIRTH",NULL,&b))return 0;
    for(SpbDevice *d=s->devices;d;d=d->next)d->born=0;
    s->phase=3;s->rebirth=0;s->rebirth_at=mg_millis();s->scan_at=0;
    printf("[Sparkplug] NBIRTH | 分组=%s | 网关=%s | bdSeq=%u\n",m->config.group_id,m->config.gateway_id,s->bdseq);
    return 1;
}
typedef struct {const char *name,*path,*unit;unsigned type;} MetricDef;
static const MetricDef definitions[]={
    {"Device/MAC",NULL,NULL,SPB_STRING},{"Device/Name",NULL,NULL,SPB_STRING},
    {"Batch/Id","batch_id",NULL,SPB_UINT32},{"Batch/MessageId","message_id",NULL,SPB_STRING},
    {"Batch/Revision","revision",NULL,SPB_UINT64},{"Batch/ReceivedAt","received_at","ms",SPB_DATETIME},
    {"Batch/Complete","collection_complete",NULL,SPB_BOOLEAN},{"Capture/Complete","capture_status.complete",NULL,SPB_BOOLEAN},
    {"Temperature","measurements.temperature_c","C",SPB_DOUBLE},
    {"Magnetic/X","measurements.magnetic_ut.x","uT",SPB_DOUBLE},
    {"Magnetic/Y","measurements.magnetic_ut.y","uT",SPB_DOUBLE},
    {"Magnetic/Z","measurements.magnetic_ut.z","uT",SPB_DOUBLE},
    {"Sound/Digital","measurements.digital_mic_db_spl","dB SPL",SPB_DOUBLE},
    {"Sound/Ultrasonic","measurements.ultrasonic_mic_db_spl","dB SPL",SPB_DOUBLE},
    {"Vibration/SampleRate","vibration.sample_rate_hz","Hz",SPB_UINT32},
    {"Vibration/SampleCount","vibration.sample_count",NULL,SPB_UINT32},
    {"Vibration/X/RMS","vibration.features.x.rms_g","g",SPB_DOUBLE},
    {"Vibration/X/Peak","vibration.features.x.peak_g","g",SPB_DOUBLE},
    {"Vibration/X/PeakToPeak","vibration.features.x.peak_to_peak_g","g",SPB_DOUBLE},
    {"Vibration/X/Kurtosis","vibration.features.x.kurtosis",NULL,SPB_DOUBLE},
    {"Vibration/X/DominantFrequency","vibration.features.x.dominant_frequency_hz","Hz",SPB_DOUBLE},
    {"Vibration/Y/RMS","vibration.features.y.rms_g","g",SPB_DOUBLE},
    {"Vibration/Y/Peak","vibration.features.y.peak_g","g",SPB_DOUBLE},
    {"Vibration/Y/PeakToPeak","vibration.features.y.peak_to_peak_g","g",SPB_DOUBLE},
    {"Vibration/Y/Kurtosis","vibration.features.y.kurtosis",NULL,SPB_DOUBLE},
    {"Vibration/Y/DominantFrequency","vibration.features.y.dominant_frequency_hz","Hz",SPB_DOUBLE},
    {"Vibration/Z/RMS","vibration.features.z.rms_g","g",SPB_DOUBLE},
    {"Vibration/Z/Peak","vibration.features.z.peak_g","g",SPB_DOUBLE},
    {"Vibration/Z/PeakToPeak","vibration.features.z.peak_to_peak_g","g",SPB_DOUBLE},
    {"Vibration/Z/Kurtosis","vibration.features.z.kurtosis",NULL,SPB_DOUBLE},
    {"Vibration/Z/DominantFrequency","vibration.features.z.dominant_frequency_hz","Hz",SPB_DOUBLE},
    {"Waveform/Available","waveform.available",NULL,SPB_BOOLEAN},
    {"Waveform/Encoding","waveform.encoding",NULL,SPB_STRING},
    {"Waveform/Scale","waveform.scale_g_per_lsb","g/LSB",SPB_DOUBLE},
    {"Waveform/CRC32","waveform.crc32",NULL,SPB_STRING},
    {"Waveform/Size","waveform.bytes","bytes",SPB_UINT32},
    {"Waveform/Samples",NULL,NULL,SPB_BYTES}
};
static const cJSON *path_value(const cJSON *json,const char *path) {
    char key[80];const char *p=path;
    while(json && p && *p){const char *dot=strchr(p,'.');size_t n=dot?(size_t)(dot-p):strlen(p);
        if(n>=sizeof(key)) {
            return NULL;
        }
        memcpy(key,p,n);key[n]=0;
        json=cJSON_GetObjectItemCaseSensitive(json,key);p=dot?dot+1:NULL;
    }
    return json;
}
static cJSON *snapshot(GatewayMqtt *m,const char *mac,sqlite3_int64 row,int *historical) {
    sqlite3_stmt *stmt=NULL;cJSON *json=NULL;
    const char *sql=row?"SELECT canonical,id,id<(SELECT max(id) FROM mqtt_outbox WHERE target=?1 AND mac=?2) FROM mqtt_outbox WHERE target=?1 AND mac=?2 AND id=?3;":
        "SELECT canonical,id,0 FROM mqtt_outbox WHERE target=?1 AND mac=?2 ORDER BY id DESC LIMIT 1;";
    if(sqlite3_prepare_v2(m->db,sql,-1,&stmt,NULL)!=SQLITE_OK)return NULL;
    sqlite3_bind_text(stmt,1,m->target,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,mac,-1,SQLITE_TRANSIENT);
    if(row)sqlite3_bind_int64(stmt,3,row);
    if(sqlite3_step(stmt)==SQLITE_ROW) {
        json=cJSON_Parse((const char *)sqlite3_column_text(stmt,0));sqlite3_int64 id=sqlite3_column_int64(stmt,1);
        if(historical)*historical=sqlite3_column_int(stmt,2);
        if(json){char compact[17],message[192];device_id(mac,compact);
            const cJSON *batch=cJSON_GetObjectItemCaseSensitive(json,"batch_id");
            snprintf(message,sizeof(message),"%s/%s/%u/%lld",m->config.gateway_id,compact,batch?(uint32_t)batch->valuedouble:0,(long long)id);
            if(!cJSON_AddStringToObject(json,"message_id",message) || !cJSON_AddNumberToObject(json,"revision",(double)id)){cJSON_Delete(json);json=NULL;}
        }
    }
    sqlite3_finalize(stmt);return json;
}
static int encode_device(GatewayMqtt *m,SpbBuffer *b,const char *mac,const cJSON *json,
                         int birth,int historical,const MqttDelivery *raw) {
    uint64_t now=utc_ms(),stamp=now;const cJSON *received=path_value(json,"received_at");
    if(cJSON_IsNumber(received) && received->valuedouble>=0)stamp=(uint64_t)received->valuedouble*1000U;
    spb_begin(b,now,(int)m->sparkplug->sequence);
    char name[256]="";sqlite3_stmt *stmt=NULL;
    if(birth && sqlite3_prepare_v2(m->db,"SELECT COALESCE(custom_name,name,mac) FROM devices WHERE mac=?;",-1,&stmt,NULL)==SQLITE_OK) {
        sqlite3_bind_text(stmt,1,mac,-1,SQLITE_TRANSIENT);
        if(sqlite3_step(stmt)==SQLITE_ROW)snprintf(name,sizeof(name),"%s",sqlite3_column_text(stmt,0));
    }
    sqlite3_finalize(stmt);
    for(unsigned i=0;i<sizeof(definitions)/sizeof(definitions[0]);i++) {
        const MetricDef *d=&definitions[i];
        if(raw && i!=2 && i!=3 && i!=4 && i!=5 && d->type!=SPB_BYTES)continue;
        const cJSON *v=d->path?path_value(json,d->path):NULL;
        SpbMetric metric={.name=d->name,.alias=10U+i,.datatype=d->type,.unit=d->unit,
            .timestamp=stamp,.historical=historical,.is_null=1};
        if(i<2){metric.data=i?name:mac;metric.length=strlen(metric.data);metric.is_null=0;}
        else if(raw && d->type==SPB_BYTES){metric.data=raw->payload;metric.length=raw->length;metric.is_null=0;}
        else if(d->type==SPB_BOOLEAN && (cJSON_IsBool(v) || (i==7 && json && !v))) {
            metric.integer=i==7 && !v?1:cJSON_IsTrue(v);metric.is_null=0;
        } else if(cJSON_IsString(v) && d->type==SPB_STRING) {
            metric.data=v->valuestring;metric.length=strlen(v->valuestring);metric.is_null=0;
        } else if(cJSON_IsNumber(v) && d->type!=SPB_BYTES && d->type!=SPB_STRING && d->type!=SPB_BOOLEAN) {
            metric.number=v->valuedouble;
            if(d->type!=SPB_DOUBLE && (v->valuedouble<0 || v->valuedouble>9007199254740991.0)){b->failed=1;break;}
            if(d->type!=SPB_DOUBLE)metric.integer=(uint64_t)v->valuedouble*(d->type==SPB_DATETIME?1000U:1U);
            metric.is_null=0;
        }
        if(!spb_add(b,&metric,birth))return 0;
    }
    return !b->failed;
}
static SpbDevice *find_device(GatewayMqtt *m,const char *mac) {
    struct GatewaySparkplug *s=m->sparkplug;unsigned count=0;
    for(SpbDevice *d=s->devices;d;d=d->next){if(!strcmp(d->mac,mac))return d;count++;}
    if(count>=1024)return NULL;
    SpbDevice *d=calloc(1,sizeof(*d));if(!d)return NULL;
    snprintf(d->mac,sizeof(d->mac),"%s",mac);d->next=s->devices;s->devices=d;return d;
}
static int device_online(GatewayMqtt *m,const char *mac) {
    sqlite3_stmt *stmt=NULL;int online=0;
    if(sqlite3_prepare_v2(m->db,"SELECT 1 FROM devices WHERE mac=? AND status='online';",-1,&stmt,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(stmt,1,mac,-1,SQLITE_TRANSIENT);int rc=sqlite3_step(stmt);
    if(rc==SQLITE_ROW)online=1;else if(rc!=SQLITE_DONE)online=-1;
    sqlite3_finalize(stmt);return online;
}
static int birth_device(GatewayMqtt *m,SpbDevice *d) {
    cJSON *json=snapshot(m,d->mac,0,NULL);SpbBuffer b;
    encode_device(m,&b,d->mac,json,1,0,NULL);cJSON_Delete(json);
    if(!publish(m,"DBIRTH",d->mac,&b))return 0;
    d->born=1;return 1;
}
/* Reconcile connectivity separately from sampling. Sleeping but registered
 * online nodes keep their metrics; only the existing offline policy kills them. */
static int sync_devices(GatewayMqtt *m) {
    struct GatewaySparkplug *s=m->sparkplug;sqlite3_stmt *stmt=NULL;
    for(SpbDevice *d=s->devices;d;d=d->next)d->seen=0;
    if(sqlite3_prepare_v2(m->db,"SELECT mac FROM devices WHERE status='online' ORDER BY mac;",-1,&stmt,NULL)!=SQLITE_OK)return -1;
    int rc,action=0;
    while((rc=sqlite3_step(stmt))==SQLITE_ROW){const char *mac=(const char *)sqlite3_column_text(stmt,0);
        SpbDevice *d=find_device(m,mac);if(!d){action=-1;break;}d->seen=1;
    }
    sqlite3_finalize(stmt);if(action<0 || rc!=SQLITE_DONE)return -1;
    for(SpbDevice *d=s->devices;d;d=d->next)if(d->born && !d->seen) {
        SpbBuffer b;spb_begin(&b,utc_ms(),(int)s->sequence);
        if(!publish(m,"DDEATH",d->mac,&b)) {
            return -1;
        }
        d->born=0;return 1;
    }
    for(SpbDevice *d=s->devices;d;d=d->next)if(!d->born && d->seen)return birth_device(m,d)?1:-1;
    /* Deleted/offline devices do not consume the bounded session cache. */
    SpbDevice **p=&s->devices;
    while(*p){SpbDevice *d=*p;if(!d->seen && !d->born){*p=d->next;free(d);}else p=&d->next;}
    return 0;
}
void gateway_sparkplug_poll(GatewayMqtt *m,uint64_t now) {
    struct GatewaySparkplug *s=m->sparkplug;struct mg_connection *c=m->connection;if(!s || !c)return;
    if(s->phase==1){if(now>=s->deadline)fail(m,"subscription_timeout","等待 Sparkplug 命令主题订阅确认超时");return;}
    if(s->sending) {
        if(c->send.len){if(now>=m->deadline)fail(m,"send_timeout","Sparkplug 数据发送超时，本地任务保留");return;}
        int complete=mqtt_store_ack(m->db,m->flight.id,m->flight.waveform);
        if(complete<0){fail(m,"storage_error","Sparkplug 发送状态保存失败，保留任务并重试");return;}
        if(complete)printf("[Sparkplug上传] MAC=%s | 批次=%u | 本批已写入网络连接（QoS 0，无服务器接收确认）\n",m->flight.mac,m->flight.batch);
        mqtt_delivery_free(&m->flight);s->sending=0;
    }
    if(c->send.len)return;
    if(s->phase==2 || (s->rebirth && now-s->rebirth_at>=1000U)) {
        if(!node_birth(m)) {
            fail(m,"birth_failed","Sparkplug 网关信息编码或发送失败");
        }
        return;
    }
    if(s->phase!=3)return;
    if(now>=s->scan_at) {
        int result=sync_devices(m);if(result<0){fail(m,"device_state_error","Sparkplug 设备信息读取或发送失败");return;}
        if(result>0)return; /* finish all lifecycle events before data */
        s->scan_at=now+1000U;
        int pending=mqtt_store_pending(m->db,m->target);
        if(pending>=0 && pending!=s->pending_count) {
            SpbBuffer b;uint64_t stamp=utc_ms();spb_begin(&b,stamp,(int)s->sequence);
            SpbMetric metric={.name="Upload/PendingBatches",.alias=4,.datatype=SPB_UINT32,.integer=(unsigned)pending,.timestamp=stamp};
            spb_add(&b,&metric,0);if(!publish(m,"NDATA",NULL,&b)){fail(m,"send_failed","Sparkplug 网关状态发送失败");return;}
            s->pending_count=pending;return;
        }
    }
    if(!m->flight.id) {
        int result=mqtt_store_next_online(m->db,m->target,&m->flight);
        if(result<0)fail(m,"storage_error","Sparkplug 待上传数据读取失败");
        if(result<=0)return;
    }
    int online=device_online(m,m->flight.mac);
    if(online<0){fail(m,"storage_error","Sparkplug 设备在线状态读取失败");return;}
    if(!online){mqtt_delivery_free(&m->flight);return;}
    SpbDevice *d=find_device(m,m->flight.mac);if(!d){fail(m,"device_limit","Sparkplug 设备缓存已满");return;}
    if(!d->born){if(!birth_device(m,d))fail(m,"birth_failed","Sparkplug 设备信息编码或发送失败");return;}
    int historical=0;cJSON *json=snapshot(m,m->flight.mac,m->flight.id,&historical);
    if(!json){fail(m,"storage_error","Sparkplug 批次快照读取失败");return;}
    SpbBuffer b;encode_device(m,&b,m->flight.mac,json,0,historical,m->flight.waveform?&m->flight:NULL);cJSON_Delete(json);
    if(!publish(m,"DDATA",m->flight.mac,&b)){fail(m,"encode_failed","Sparkplug 批次数据编码或发送失败");return;}
    s->sending=1;m->deadline=now+m->config.ack_timeout_ms;
}
