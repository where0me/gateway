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
#include "gateway_mqtt.h"

static unsigned connects,births,publications,pings,subscriber_ready;
static void check_message(const struct mg_mqtt_message *message) {
    assert(message->topic.len>7 && !memcmp(message->topic.buf,"spBv1.0/",7));
    assert(message->qos==0 && message->data.len && (uint8_t)message->data.buf[0]!='{');
    assert(!((uint8_t)message->dgram.buf[0]&1U));
    char topic[256];assert(message->topic.len<sizeof(topic));
    memcpy(topic,message->topic.buf,message->topic.len);topic[message->topic.len]=0;
    if(strstr(topic,"/NBIRTH/"))births++;
    if(strstr(topic,"/DDATA/"))publications++;
}
static void broker_event(struct mg_connection *c,int event,void *data) {
    if(event==MG_EV_MQTT_CMD) {
        const struct mg_mqtt_message *m=data;
        if(m->cmd==MQTT_CMD_CONNECT) {
            const uint8_t ack[]={0x20,2,0,connects?0:5};
            assert(mg_send(c,ack,sizeof(ack)));connects++;
        }
        if(m->cmd==MQTT_CMD_SUBSCRIBE) {
            const uint8_t ack[]={0x90,3,(uint8_t)(m->id>>8),(uint8_t)m->id,1};
            assert(mg_send(c,ack,sizeof(ack)));
        }
        if(m->cmd==MQTT_CMD_PINGREQ){mg_mqtt_pong(c);pings++;}
    } else if(event==MG_EV_MQTT_MSG)check_message(data);
}
static void subscriber_event(struct mg_connection *c,int event,void *data) {
    if(event==MG_EV_MQTT_OPEN) {
        assert(*(const uint8_t *)data==0);
        struct mg_mqtt_opts opts={0};opts.qos=1;opts.topic=mg_str(c->fn_data);mg_mqtt_sub(c,&opts);
    } else if(event==MG_EV_MQTT_CMD) {
        const struct mg_mqtt_message *m=data;
        if(m->cmd==MQTT_CMD_SUBACK){assert((uint8_t)m->dgram.buf[m->dgram.len-1]!=0x80);subscriber_ready=1;}
    } else if(event==MG_EV_MQTT_MSG)check_message(data);
    else if(event==MG_EV_ERROR)assert(!"EMQX subscriber connection failed");
}
int main(int argc,char **argv) {
    int real=argc==3 && !strcmp(argv[1],"--real-emqx");mg_log_set(MG_LL_NONE);
    GatewayMqttConfig config;gateway_mqtt_config_defaults(&config);
    if(real){assert(!gateway_mqtt_load_config(argv[2],&config) && config.enabled);strcpy(config.url,"mqtt://127.0.0.1:1883");}
    config.enabled=1;config.keepalive_sec=5;config.connect_timeout_ms=2000;
    config.ack_timeout_ms=1000;config.max_pending_bytes=1024*1024;
    unsigned long long unique=(unsigned long long)mg_millis();
    snprintf(config.gateway_id,sizeof(config.gateway_id),"test-%llu",unique);
    snprintf(config.client_id,sizeof(config.client_id),"sparkplug-client-%llu",unique);
    strcpy(config.group_id,"isolated-tests");
    struct mg_mgr manager;mg_mgr_init(&manager);
    struct mg_connection *listener=NULL,*subscriber=NULL;
    char filter[256],subscriber_id[128];
    if(real) {
        snprintf(filter,sizeof(filter),"spBv1.0/%s/+/%s/#",config.group_id,config.gateway_id);
        snprintf(subscriber_id,sizeof(subscriber_id),"spb-subscriber-%llu",unique);
        struct mg_mqtt_opts opts={0};opts.version=4;opts.clean=true;opts.keepalive=30;opts.client_id=mg_str(subscriber_id);
        const char *user=getenv("MQTT_TEST_SUB_USER"),*password=getenv("MQTT_TEST_SUB_PASSWORD");
        opts.user=mg_str(user?user:config.username);opts.pass=mg_str(password?password:config.password);
        subscriber=mg_mqtt_connect(&manager,config.url,&opts,subscriber_event,filter);assert(subscriber);
    } else {
        listener=mg_mqtt_listen(&manager,"mqtt://127.0.0.1:0",broker_event,NULL);assert(listener);
        snprintf(config.url,sizeof(config.url),"mqtt://127.0.0.1:%u",(unsigned)mg_ntohs(listener->loc.port));subscriber_ready=1;
    }
    const char *mac="01:02:03:04:05:06:07:08";
    assert(!init_database() && !insert_new_device(mac,"Sparkplug test"));
    assert(sqlite3_exec(g_db,"UPDATE devices SET status='online';",NULL,NULL,NULL)==SQLITE_OK);
    uint8_t raw[12]={1,0,2,0,3,0,4,0,5,0,6,0};int32_t features[3][5]={{1000}};
    assert(record_collection_temp(mac,42,26.31f)==1 && record_collection_mag(mac,42,1,2,3)==1);
    assert(record_collection_sound(mac,42,1,67)==1 && record_collection_sound(mac,42,0,68)==1);
    assert(!save_vibration_batch(mac,42,26667,2,raw,sizeof(raw),features));
    GatewayMqtt mqtt;assert(!gateway_mqtt_init(&mqtt,&manager,g_db,&config));
    int done=0,ping_forced=0;uint64_t until=mg_millis()+15000;
    while(mg_millis()<until) {
        mg_mgr_poll(&manager,5);if(subscriber_ready)gateway_mqtt_poll(&mqtt,mg_millis());
        if(births && publications==2 && !mqtt_store_pending(g_db,mqtt.target)) {
            if(real){done=1;break;}
            if(!ping_forced){mqtt.ping_at=0;ping_forced=1;}
            if(pings && !mqtt.pong_deadline){done=1;break;}
        }
    }
    assert(done && subscriber_ready && !mqtt_store_pending(g_db,mqtt.target));
    if(!real)assert(connects==2 && births==1 && publications==2 && pings);
    gateway_mqtt_stop(&mqtt);if(listener)listener->is_closing=1;if(subscriber)subscriber->is_closing=1;
    mg_mgr_free(&manager);close_database();
    puts("Sparkplug-only client: auth refusal/retry, subscription, Protobuf DDATA result/raw, no JSON/PUBACK dependency, keepalive PASS");return 0;
}
