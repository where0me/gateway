#include "gateway_mqtt.h"
#include "gateway_sparkplug.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

static int text_option(const cJSON *object,const char *key,char *out,size_t size) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(object,key);
    if(!v)return 1;
    if(!cJSON_IsString(v) || strlen(v->valuestring)>=size)return 0;
    memcpy(out,v->valuestring,strlen(v->valuestring)+1);return 1;
}
static int token(const char *s,int slashes) {
    if(!s[0] || s[0]=='/' || s[strlen(s)-1]=='/')return 0;
    for(size_t i=0;s[i];i++)if(!(isalnum((unsigned char)s[i]) || s[i]=='_' || s[i]=='-' || s[i]=='.' || (slashes && s[i]=='/')))return 0;
    return !strstr(s,"//");
}
static int integer_option(const cJSON *object,const char *key,unsigned *out,unsigned min,unsigned max) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(object,key);
    if(!v)return 1;
    if(!cJSON_IsNumber(v) || v->valuedouble<min || v->valuedouble>max || v->valuedouble!=(unsigned)v->valuedouble)return 0;
    *out=(unsigned)v->valuedouble;return 1;
}
void gateway_mqtt_config_defaults(GatewayMqttConfig *out) {
    memset(out,0,sizeof(*out));strcpy(out->gateway_id,"gw001");
    strcpy(out->protocol,"sparkplug_b");strcpy(out->group_id,"factory01");
    strcpy(out->client_id,"gateway-gw001");out->client_id_auto=1;
    out->keepalive_sec=60;out->ack_timeout_ms=15000;out->connect_timeout_ms=10000;
    out->max_pending_bytes=512LL*1024*1024;
}
static int valid_url(const char *url) {
    if(strncmp(url,"mqtt://",7) || !url[7])return 0;
    const char *host=url+7,*port=NULL,*end=url+strlen(url);
    for(const unsigned char *p=(const unsigned char *)host;*p;p++)
        if(isspace(*p) || *p<33 || *p>=127 || strchr("/@?#\\",*p))return 0;
    if(*host=='[') {
        const char *close=strchr(host,']');struct mg_addr ip;
        if(!close || !mg_aton(mg_str_n(host,(size_t)(close-host+1)),&ip) || !ip.is_ip6)return 0;
        if(close[1]) {if(close[1]!=':')return 0;port=close+2;}
    } else {
        const char *colon=strchr(host,':');const char *host_end=colon?colon:end;
        if(host_end==host)return 0;
        for(const char *p=host;p<host_end;p++)if(!isalnum((unsigned char)*p) && *p!='-' && *p!='.')return 0;
        if(colon)port=colon+1;
    }
    if(port) {
        unsigned value=0;if(!*port)return 0;
        for(const char *p=port;*p;p++) {if(!isdigit((unsigned char)*p))return 0;value=value*10U+(unsigned)(*p-'0');if(value>65535)return 0;}
        if(!value)return 0;
    }
    return 1;
}
int gateway_mqtt_config_valid(const GatewayMqttConfig *c) {
    return !strcmp(c->protocol,"sparkplug_b") && token(c->group_id,0) &&
        (c->enabled==0 || c->enabled==1) && token(c->gateway_id,0) && token(c->client_id,0) &&
        ((!c->enabled && !c->url[0]) || valid_url(c->url)) && c->keepalive_sec>=5 && c->keepalive_sec<=3600 &&
        c->ack_timeout_ms>=1000 && c->ack_timeout_ms<=300000 && c->connect_timeout_ms>=1000 && c->connect_timeout_ms<=120000 &&
        c->max_pending_bytes>=1048576 && c->max_pending_bytes<=1099511627776LL;
}
int gateway_mqtt_config_parse(const cJSON *root,GatewayMqttConfig *out,int environment) {
    gateway_mqtt_config_defaults(out);
    if(!cJSON_IsObject(root))return -1;
    const cJSON *m=cJSON_GetObjectItemCaseSensitive(root,"mqtt");
    if(!m)return 0;
    const cJSON *enabled=cJSON_GetObjectItemCaseSensitive(m,"enabled");
    int valid=cJSON_IsObject(m) && (!enabled || cJSON_IsBool(enabled));
    out->enabled=cJSON_IsTrue(enabled);
    valid=valid && text_option(m,"url",out->url,sizeof(out->url)) &&
        text_option(m,"protocol",out->protocol,sizeof(out->protocol)) &&
        text_option(m,"group_id",out->group_id,sizeof(out->group_id)) &&
        text_option(m,"gateway_id",out->gateway_id,sizeof(out->gateway_id)) &&
        text_option(m,"client_id",out->client_id,sizeof(out->client_id)) &&
        text_option(m,"username",out->username,sizeof(out->username)) &&
        text_option(m,"password",out->password,sizeof(out->password)) &&
        integer_option(m,"keepalive_sec",&out->keepalive_sec,5,3600) &&
        integer_option(m,"ack_timeout_ms",&out->ack_timeout_ms,1000,300000) &&
        integer_option(m,"connect_timeout_ms",&out->connect_timeout_ms,1000,120000);
    /* File compatibility only: legacy JSON/missing protocol now uses Sparkplug.
     * The management API rejects explicit attempts to select JSON. */
    if(!out->protocol[0] || !strcmp(out->protocol,"json"))strcpy(out->protocol,"sparkplug_b");
    const cJSON *quota=cJSON_GetObjectItemCaseSensitive(m,"max_pending_bytes");
    if(quota) {
        if(!cJSON_IsNumber(quota) || quota->valuedouble<1048576 || quota->valuedouble>1099511627776.0 ||
           quota->valuedouble!=(double)(sqlite3_int64)quota->valuedouble)valid=0;
        else out->max_pending_bytes=(sqlite3_int64)quota->valuedouble;
    }
    const char *user=environment?getenv("GATEWAY_MQTT_USERNAME"):NULL,*password=environment?getenv("GATEWAY_MQTT_PASSWORD"):NULL;
    if(user) {if(strlen(user)>=sizeof(out->username))valid=0;else strcpy(out->username,user);}
    if(password) {if(strlen(password)>=sizeof(out->password))valid=0;else strcpy(out->password,password);}
    const cJSON *client=cJSON_GetObjectItemCaseSensitive(m,"client_id");
    out->client_id_auto=!client || (cJSON_IsString(client) && !client->valuestring[0]);
    if(out->client_id_auto)snprintf(out->client_id,sizeof(out->client_id),"gateway-%s",out->gateway_id);
    if(!valid || !gateway_mqtt_config_valid(out)){gateway_mqtt_config_defaults(out);return -1;}return 0;
}
int gateway_mqtt_load_config(const char *path,GatewayMqttConfig *out) {
    gateway_mqtt_config_defaults(out);
    FILE *file=fopen(path,"rb");if(!file)return -1;
    char *text=malloc(65537);if(!text){fclose(file);return -1;}
    size_t n=fread(text,1,65536,file);int error=ferror(file),extra=fgetc(file);fclose(file);text[n]=0;
    if(error || extra!=EOF){free(text);return -1;}
    cJSON *root=cJSON_ParseWithLengthOpts(text,n+1,NULL,1);free(text);
    int result=gateway_mqtt_config_parse(root,out,1);cJSON_Delete(root);return result;
}
static void mqtt_error(GatewayMqtt *m,const char *code,const char *message) {
    snprintf(m->error_code,sizeof(m->error_code),"%s",code);
    snprintf(m->error_message,sizeof(m->error_message),"%s",message);m->error_at=(int64_t)time(NULL);
}
static void reconnect_later(GatewayMqtt *m,uint64_t now) {
    gateway_sparkplug_clear(m);
    m->ready=0;m->pong_deadline=0;mqtt_delivery_free(&m->flight);
    m->reconnect_at=now+m->backoff_ms;
    if(m->backoff_ms<30000)m->backoff_ms=m->backoff_ms>15000?30000:m->backoff_ms*2;
}
static void mqtt_event(struct mg_connection *c,int event,void *event_data) {
    GatewayMqtt *m=c->fn_data;uint64_t now=mg_millis();
    if(m->connection!=c || m->stopping)return;
    if(event==MG_EV_MQTT_OPEN) {
        /* This repository's mongoose.c passes uint8_t*, not the int* in an
         * older API comment. Follow the actual implementation. */
        unsigned status=*(const uint8_t *)event_data;
        if(status) {mqtt_error(m,status==4?"bad_credentials":status==5?"not_authorized":"connection_rejected",
            status==4?"用户名或密码错误":status==5?"服务器拒绝访问，请检查账号权限":"服务器拒绝 MQTT 连接");
            fprintf(stderr,"[MQTT] 认证/连接被拒绝，CONNACK=%u；保留待发送数据\n",status);c->is_closing=1;return;}
        m->ready=1;m->backoff_ms=1000;m->ping_at=now+(uint64_t)m->config.keepalive_sec*500;
        m->error_code[0]=m->error_message[0]=0;m->error_at=0;
        gateway_sparkplug_open(m,now);
        printf("[MQTT] 已连接 | 网关=%s | 待上传批次=%d\n",m->config.gateway_id,mqtt_store_pending(m->db,m->target));
    } else if(event==MG_EV_MQTT_CMD) {
        const struct mg_mqtt_message *message=event_data;
        gateway_sparkplug_event(m,event,message);
        if(message->cmd==MQTT_CMD_PINGRESP)m->pong_deadline=0;
    } else if(event==MG_EV_MQTT_MSG) {
        gateway_sparkplug_event(m,event,event_data);
    } else if(event==MG_EV_ERROR) {
        mqtt_error(m,"transport_error","无法连接或传输中断，请检查服务器地址、端口和网络");
        /* Avoid emitting a URL containing credentials or any library dump. */
        if(!m->last_error || now-m->last_error>=10000) {fprintf(stderr,"[MQTT] 连接/传输错误；本地任务保留并自动重试\n");m->last_error=now;}
        c->is_closing=1;
    } else if(event==MG_EV_CLOSE && m->connection==c) {
        if(m->ready && !m->error_code[0])mqtt_error(m,"disconnected","服务器连接已断开，正在自动重连");
        m->connection=NULL;reconnect_later(m,now);
    }
}
int gateway_mqtt_init(GatewayMqtt *m,struct mg_mgr *mgr,sqlite3 *db,const GatewayMqttConfig *config) {
    memset(m,0,sizeof(*m));m->config=*config;m->mgr=mgr;m->db=db;m->backoff_ms=1000;
    m->last_unknown=-1;
    m->store_ready=mqtt_store_init(db)==0;
    if(!m->store_ready) {fprintf(stderr,"[MQTT] 本地队列初始化失败，MQTT已停用；Zigbee继续运行\n");m->config.enabled=0;return -1;}
    if(gateway_mqtt_prepare(m,config,m->target)!=0) {m->config.enabled=0;mqtt_error(m,"storage_error","上传队列不可用");return -1;}
    if(!config->enabled){printf("[MQTT] 未启用；批次接收记录仍保存在本地\n");return 0;}
    printf("[MQTT] 已启用 | 网关=%s | 模式=Sparkplug B / Protobuf / QoS 0 | 历史完整批次补发\n",config->gateway_id);
    return 0;
}
void gateway_mqtt_poll(GatewayMqtt *m,uint64_t now) {
    if(m->stopping || !m->config.enabled || !m->store_ready)return;
    if(now>=m->reconcile_at) {
        int result=mqtt_store_reconcile(m->db,m->target,m->config.gateway_id,m->config.group_id,m->config.max_pending_bytes);
        m->reconcile_at=now+(result==2?5000:200);
        if((result<0 || result==2) && (!m->last_error || now-m->last_error>=10000)) {
            fprintf(stderr,"[MQTT] %s；原始数据库记录保留，不丢弃待上传数据\n",result==2?"待发队列达到容量限制":"整理上传批次失败");m->last_error=now;
        }
    }
    if(now>=m->status_at) {
        m->status_at=now+10000;
        sqlite3_stmt *s=NULL;
        if(sqlite3_prepare_v2(m->db,"SELECT count(*) FROM mqtt_batch_state WHERE reason='legacy_completion_unknown';",-1,&s,NULL)==SQLITE_OK &&
           sqlite3_step(s)==SQLITE_ROW) {
            int unknown=sqlite3_column_int(s,0);
            if(unknown && unknown!=m->last_unknown)
                printf("[MQTT] %d个历史无波形批次缺少完成依据，保留待核验，未按完整批次发布\n",unknown);
            m->last_unknown=unknown;
        }
        sqlite3_finalize(s);
    }
    if(!m->connection && now>=m->reconnect_at) {
        struct mg_mqtt_opts opts={0};opts.version=4;opts.clean=true;opts.keepalive=(uint16_t)m->config.keepalive_sec;
        opts.client_id=mg_str(m->config.client_id);opts.user=mg_str(m->config.username);opts.pass=mg_str(m->config.password);
        if(!gateway_sparkplug_options(m,&opts)) {
            mqtt_error(m,"session_storage_failed","Sparkplug 会话序号保存或离线消息编码失败");reconnect_later(m,now);return;
        }
        m->connection=mg_mqtt_connect(m->mgr,m->config.url,&opts,mqtt_event,m);
        m->deadline=now+m->config.connect_timeout_ms;
        if(!m->connection){mqtt_error(m,"connect_failed","无法创建 MQTT 连接，正在重试");reconnect_later(m,now);}
    }
    struct mg_connection *c=m->connection;if(!c || c->is_closing)return;
    if(!m->ready) {if(now>=m->deadline){mqtt_error(m,"connect_timeout","连接服务器超时，请检查地址、端口和网络");c->is_closing=1;}return;}
    if(m->pong_deadline && now>=m->pong_deadline) {mqtt_error(m,"keepalive_timeout","服务器心跳回复超时，正在重连");c->is_closing=1;return;}
    if(now>=m->ping_at && !m->pong_deadline) {
        mg_mqtt_ping(c);m->pong_deadline=now+m->config.keepalive_sec*1000U;
        m->ping_at=now+m->config.keepalive_sec*500U;
    }
    gateway_sparkplug_poll(m,now);
}
void gateway_mqtt_stop(GatewayMqtt *m) {
    m->stopping=1;
    if(m->connection)m->connection->is_closing=1;
    m->connection=NULL;m->ready=0;
    mqtt_delivery_free(&m->flight);
    /* No MQTT DISCONNECT: closing the socket causes the broker to deliver the
     * registered NDEATH even when the process cannot drain a graceful publish. */
    gateway_sparkplug_clear(m);
}
int gateway_mqtt_prepare(GatewayMqtt *m,const GatewayMqttConfig *config,char target[768]) {
    target[0]=0;
    if(!gateway_mqtt_config_valid(config) || !m->store_ready)return -1;
    if(!config->url[0])return 0;
    int n=snprintf(target,768,"%s|%s|%s|sparkplug_b:%s",config->url,config->username,config->gateway_id,config->group_id);
    return n<0 || n>=768 || mqtt_store_target(m->db,target)!=0?-1:0;
}
void gateway_mqtt_apply(GatewayMqtt *m,const GatewayMqttConfig *config,const char *target) {
    if(m->connection)m->connection->is_closing=1;
    m->connection=NULL;m->ready=0;mqtt_delivery_free(&m->flight);
    gateway_sparkplug_clear(m);
    m->config=*config;snprintf(m->target,sizeof(m->target),"%s",target);
    m->stopping=0;m->deadline=m->reconnect_at=m->reconcile_at=m->ping_at=m->pong_deadline=0;
    m->backoff_ms=1000;m->error_code[0]=m->error_message[0]=0;m->error_at=0;
}
