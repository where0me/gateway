#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#define GATEWAY_DATABASE_PATH ":memory:"
#include "../gateway-core/src/db_manager.c"
#include "gateway_mqtt_api.h"
#include "cJSON.h"

static struct mg_mgr manager;
static GatewayMqtt mqtt;
static GatewayMqttApi api;
static unsigned connack,hold_ack,connects,collections,waveforms;
static char client_ids[32][128];
static const uint8_t raw[]={1,0,2,0,3,0,4,0,5,0,6,0};
static volatile sig_atomic_t stopped;
static void stop_signal(int signal_number){(void)signal_number;stopped=1;}
static void broker(struct mg_connection *c,int event,void *data) {
    if(event==MG_EV_MQTT_CMD) {
        const struct mg_mqtt_message *m=data;
        if(m->cmd==MQTT_CMD_CONNECT) {
            /* Parse the real MQTT 3.1.1 CONNECT payload to check unique IDs. */
            const uint8_t *b=(const uint8_t *)m->dgram.buf;size_t pos=1;
            while(pos<m->dgram.len && (b[pos++]&128)){}
            pos+=10;assert(pos+2<=m->dgram.len);
            unsigned n=((unsigned)b[pos]<<8)|b[pos+1];pos+=2;assert(n<128 && pos+n<=m->dgram.len);
            if(connects<32){memcpy(client_ids[connects],b+pos,n);client_ids[connects][n]=0;}connects++;
            if(!hold_ack){uint8_t ack[]={0x20,2,0,(uint8_t)connack};assert(mg_send(c,ack,sizeof(ack)));}
        }
        if(m->cmd==MQTT_CMD_PINGREQ)mg_mqtt_pong(c);
        if(m->cmd==MQTT_CMD_SUBSCRIBE) {
            const uint8_t ack[]={0x90,3,(uint8_t)(m->id>>8),(uint8_t)m->id,1};assert(mg_send(c,ack,sizeof(ack)));
        }
    } else if(event==MG_EV_MQTT_MSG) {
        const struct mg_mqtt_message *m=data;
        char topic[256];assert(m->topic.len<sizeof(topic));memcpy(topic,m->topic.buf,m->topic.len);topic[m->topic.len]=0;
        assert(!strncmp(topic,"spBv1.0/",7) && m->qos==0 && m->data.len && m->data.buf[0]!='{');
        if(strstr(topic,"/DDATA/")) {
            /* The small raw fixture is inside a Protobuf Bytes metric, never
             * a separate unframed binary MQTT publication. */
            int has_raw=0;for(size_t i=0;i+sizeof(raw)<=m->data.len;i++)
                if(!memcmp(m->data.buf+i,raw,sizeof(raw)))has_raw=1;
            if(has_raw)waveforms++;else collections++;
        }
    }
}
static cJSON *request(const char *method,const char *path,const char *body,const char *origin,const char *type,int expected) {
    char message[12000];
    int n=snprintf(message,sizeof(message),"%s %s HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n%s%s%sContent-Type: %s\r\nContent-Length: %zu\r\n\r\n%s",
        method,path,origin?"Origin: ":"",origin?origin:"",origin?"\r\n":"",type?type:"application/json",body?strlen(body):0,body?body:"");
    assert(n>0 && (size_t)n<sizeof(message));
    struct mg_http_message hm;assert(mg_http_parse(message,(size_t)n,&hm)>0);
    struct mg_connection c;memset(&c,0,sizeof(c));c.mgr=&manager;
    assert(gateway_mqtt_api_handle(&api,&c,&hm)==1);
    struct mg_http_message response;assert(mg_http_parse((const char *)c.send.buf,c.send.len,&response)>0);
    assert(mg_http_status(&response)==expected);
    struct mg_str *cache=mg_http_get_header(&response,"Cache-Control");
    assert(cache && mg_strcmp(*cache,mg_str("no-store"))==0);
    if(expected!=200 || strcmp(path,"/api/mqtt/settings")) {
        assert(!strstr((const char *)c.send.buf,"test-private-password"));
        assert(!strstr((const char *)c.send.buf,"new-private-password"));
    }
    assert(!strstr((const char *)c.send.buf,"env-private-password"));
    cJSON *root=expected==204?NULL:cJSON_ParseWithLength(response.body.buf,response.body.len);assert(expected==204 || root);
    mg_iobuf_free(&c.send);return root;
}
static void call(const char *method,const char *path,const char *body,int status) {cJSON_Delete(request(method,path,body,NULL,NULL,status));}
static char *read_text(const char *path) {
    FILE *f=fopen(path,"rb");assert(f);char *text=calloc(1,65537);assert(text);
    assert(fread(text,1,65536,f)<65536 && !ferror(f));fclose(f);return text;
}
static void write_text(const char *path,const char *text) {
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(text,1,strlen(text),f)==strlen(text));assert(!fclose(f));
}
static void tick(unsigned duration) {
    uint64_t end=mg_millis()+duration;
    do {mg_mgr_poll(&manager,2);gateway_mqtt_poll(&mqtt,mg_millis());gateway_mqtt_api_poll(&api,mg_millis());}while(mg_millis()<end);
}
static void test_connection(unsigned status) {
    connack=status;cJSON *r=request("POST","/api/mqtt/test","{}",NULL,NULL,202);
    char path[128];snprintf(path,sizeof(path),"/api/mqtt/test?id=%s",cJSON_GetObjectItem(r,"id")->valuestring);cJSON_Delete(r);
    uint64_t end=mg_millis()+3000;
    do {tick(10);}while(!strcmp(api.probe.state,"testing") && mg_millis()<end);
    r=request("GET",path,NULL,NULL,NULL,200);
    assert(!strcmp(cJSON_GetObjectItem(r,"state")->valuestring,status?"failed":"success"));
    if(status==4)assert(!strcmp(cJSON_GetObjectItem(r,"code")->valuestring,"bad_credentials"));
    cJSON_Delete(r);connack=0;
}
static void seed_local(uint32_t batch) {
    const char *mac="01:02:03:04:05:06:07:08";
    assert(!mqtt_store_policy(g_db,mac,7));assert(!mqtt_store_note(g_db,mac,batch,MQTT_BATCH_CAPTURE));
    assert(record_collection_temp(mac,batch,26.25f)==1 && record_collection_mag(mac,batch,1,2,3)==1);
    assert(record_collection_sound(mac,batch,1,67)==1 && record_collection_sound(mac,batch,0,68)==1);
    int32_t features[3][5]={{1000}};
    assert(!save_vibration_batch(mac,batch,26667,2,raw,sizeof(raw),features));
}
static void seed(uint32_t batch) {
    seed_local(batch);
    for(unsigned attempt=0;attempt<16;attempt++) {
        int result=mqtt_store_reconcile(g_db,mqtt.target,mqtt.config.gateway_id,mqtt.config.group_id,mqtt.config.max_pending_bytes);
        assert(result>=0);if(result==1)return;
    }
    assert(!"Complete synthetic batch was not queued");
}
static void serve(struct mg_connection *c,int event,void *data) {
    if(event==MG_EV_HTTP_MSG && !gateway_mqtt_api_handle(&api,c,data))mg_http_reply(c,404,"","");
}
int main(int argc,char **argv) {
    assert(argc==2 || (argc==3 && !strcmp(argv[1],"--serve")));const char *path=argv[argc-1];
    int serving=argc==3;mg_log_set(MG_LL_NONE);mg_mgr_init(&manager);
    struct mg_connection *listener=mg_mqtt_listen(&manager,"mqtt://127.0.0.1:0",broker,NULL);assert(listener);
    unsigned port=mg_ntohs(listener->loc.port);char fixture[2048];
    snprintf(fixture,sizeof(fixture),"{\"serial_path\":\"/dev/test-only\",\"baud_rate\":921600,\"unrelated\":{\"keep\":true},"
        "\"mqtt\":{\"enabled\":false,\"url\":\"mqtt://127.0.0.1:%u\",\"username\":\"gateway\",\"password\":\"test-private-password\","
        "\"protocol\":\"json\",\"topic_prefix\":\"zigbee/tests/management\",\"connect_timeout_ms\":1000}}",port);
    write_text(path,fixture);
    assert(!init_database() && !insert_new_device("01:02:03:04:05:06:07:08","MQTT management test"));
    assert(sqlite3_exec(g_db,"UPDATE devices SET status='online';",NULL,NULL,NULL)==SQLITE_OK);
    GatewayMqttConfig config;assert(!gateway_mqtt_load_config(path,&config));
    assert(!strcmp(config.protocol,"sparkplug_b") && !strcmp(config.group_id,"factory01"));
    assert(!gateway_mqtt_init(&mqtt,&manager,g_db,&config));gateway_mqtt_api_init(&api,&mqtt,path);
    if(serving) {
        struct mg_connection *http=mg_http_listen(&manager,"http://127.0.0.1:8080",serve,NULL);assert(http);
        seed(42);signal(SIGINT,stop_signal);signal(SIGTERM,stop_signal);
        puts("Isolated MQTT settings API at 127.0.0.1:8080; real local test broker, synthetic in-memory batches, no device access.");fflush(stdout);
        while(!stopped)tick(20);
        gateway_mqtt_api_stop(&api);gateway_mqtt_stop(&mqtt);mg_mgr_free(&manager);close_database();return 0;
    }
    cJSON *r=request("GET","/api/mqtt/settings",NULL,"http://localhost:5178",NULL,200);
    cJSON *v=cJSON_GetObjectItem(r,"config");
    assert(!strcmp(cJSON_GetObjectItem(v,"password")->valuestring,"test-private-password"));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(v,"password_set")) && !strcmp(cJSON_GetObjectItem(v,"client_id")->valuestring,""));
    cJSON_Delete(r);
    call("POST","/api/mqtt/settings","{\"protocol\":\"sparkplug_b\",\"group_id\":\"line01\"}",200);
    assert(!strcmp(mqtt.config.protocol,"sparkplug_b") && !strcmp(mqtt.config.group_id,"line01"));
    r=request("GET","/api/mqtt/settings",NULL,NULL,NULL,200);
    assert(!strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(r,"config"),"protocol")->valuestring,"sparkplug_b"));
    assert(!strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(r,"runtime"),"delivery_semantics")->valuestring,"socket_sent"));
    cJSON_Delete(r);assert(!gateway_mqtt_load_config(path,&config) && !strcmp(config.group_id,"line01"));
    call("POST","/api/mqtt/settings","{\"protocol\":\"sparkplug_b\",\"group_id\":\"bad/group\"}",400);
    char *unchanged=read_text(path);
    call("POST","/api/mqtt/settings","{\"protocol\":\"json\"}",400);
    assert(!strcmp(mqtt.config.protocol,"sparkplug_b") && !strcmp(mqtt.config.password,"test-private-password"));
    char *still=read_text(path);assert(!strcmp(unchanged,still));free(unchanged);free(still);
    call("GET","/api/mqtt/status",NULL,200);
    cJSON_Delete(request("GET","/api/mqtt/settings",NULL,"https://evil.example",NULL,403));
    cJSON_Delete(request("POST","/api/mqtt/settings","{}",NULL,"text/plain",415));
    call("POST","/api/mqtt/settings","{\"password\":\"a\",\"clear_password\":true}",400);
    call("POST","/api/mqtt/settings","{\"enabled\":true,\"enabled\":false}",400);
    call("POST","/api/mqtt/settings","{\"topic_prefix\":\"changed\"}",400);
    call("POST","/api/mqtt/settings","{\"url\":\"mqtts://server:8883\"}",400);
    call("POST","/api/mqtt/settings","{\"url\":\"mqtt://server:65536\"}",400);
    call("POST","/api/mqtt/settings","{\"url\":\"mqtt://user:private@server:1883\"}",400);
    call("POST","/api/mqtt/settings","{\"password\":\"a\\u0000b\"}",400);
    call("POST","/api/mqtt/settings","{\"ack_timeout_ms\":1}",400);
    char *before=read_text(path);test_connection(0);test_connection(4);char *after=read_text(path);
    assert(!strcmp(before,after) && !mqtt.config.enabled);free(before);free(after);
    call("GET","/api/mqtt/test?id=not-current",NULL,409);
    hold_ack=1;call("POST","/api/mqtt/test","{}",202);call("POST","/api/mqtt/test","{}",409);
    api.probe.deadline=0;gateway_mqtt_api_poll(&api,mg_millis());assert(!strcmp(api.probe.code,"connect_timeout"));hold_ack=0;tick(5);
    call("POST","/api/mqtt/settings","{\"enabled\":true,\"client_id\":\"\",\"password\":\"\"}",200);
    assert(!strcmp(mqtt.config.password,"test-private-password"));tick(100);assert(mqtt.ready);
    struct mg_connection *production=mqtt.connection;test_connection(0);assert(mqtt.connection==production && mqtt.ready);
    assert(connects>=4 && strcmp(client_ids[connects-1],mqtt.config.client_id));
    before=read_text(path);write_text(path,"not valid JSON");
    call("POST","/api/mqtt/settings","{\"enabled\":false}",500);
    assert(mqtt.config.enabled && mqtt.connection==production && mqtt.ready);
    write_text(path,before);free(before);
    after=read_text(path);cJSON *file=cJSON_Parse(after);free(after);
    assert(cJSON_GetObjectItem(file,"baud_rate")->valueint==921600 && cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(file,"unrelated"),"keep")));
    assert(!strcmp(cJSON_GetObjectItem(file,"serial_path")->valuestring,"/dev/test-only"));
    cJSON *saved_mqtt=cJSON_GetObjectItem(file,"mqtt");
    assert(!strcmp(cJSON_GetObjectItem(saved_mqtt,"protocol")->valuestring,"sparkplug_b"));
    assert(!strcmp(cJSON_GetObjectItem(saved_mqtt,"group_id")->valuestring,"line01"));
    assert(!cJSON_GetObjectItem(saved_mqtt,"topic_prefix"));cJSON_Delete(file);
    seed(42);tick(300);assert(collections==1 && waveforms==1);
    MqttStoreStats stats;assert(!mqtt_store_stats(g_db,mqtt.target,&stats));
    assert(!stats.pending_batches && !stats.pending_bytes && stats.last_confirmed_at>0);
    call("POST","/api/mqtt/settings","{\"enabled\":false}",200);seed_local(43);
    assert(!mqtt_store_pending(g_db,mqtt.target)); // no disabled reconciliation
    r=request("GET","/api/mqtt/status",NULL,NULL,NULL,200);
    cJSON *backlog=cJSON_GetObjectItem(r,"runtime");
    assert(!strcmp(cJSON_GetObjectItem(backlog,"state")->valuestring,"disabled"));
    assert(cJSON_GetObjectItem(backlog,"pending_batches")->valueint==1);
    assert(cJSON_GetObjectItem(backlog,"pending_bytes")->valuedouble>sizeof(raw));
    cJSON_Delete(r);assert(!mqtt_store_pending(g_db,mqtt.target));
    for(unsigned attempt=0;attempt<16 && !mqtt_store_pending(g_db,mqtt.target);attempt++)
        assert(mqtt_store_reconcile(g_db,mqtt.target,mqtt.config.gateway_id,
            mqtt.config.group_id,mqtt.config.max_pending_bytes)>=0);
    assert(!mqtt_store_stats(g_db,mqtt.target,&stats) && stats.pending_batches==1 && stats.pending_bytes>sizeof(raw));
    call("POST","/api/mqtt/settings","{\"username\":\"other-account\"}",200);tick(10);
    assert(!mqtt_store_stats(g_db,mqtt.target,&stats) && stats.pending_batches==0 && stats.other_pending_batches==1);
    assert(!mqtt.ready && !mqtt.connection);
    call("POST","/api/mqtt/settings","{\"username\":\"gateway\"}",200);
    assert(!mqtt_store_stats(g_db,mqtt.target,&stats) && stats.pending_batches==1);
    call("POST","/api/mqtt/settings","{\"password\":\"new-private-password\"}",200);
    assert(!strcmp(mqtt.config.password,"new-private-password"));
    r=request("GET","/api/mqtt/settings",NULL,NULL,NULL,200);
    assert(!strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(r,"config"),"password")->valuestring,"new-private-password"));cJSON_Delete(r);
    call("GET","/api/mqtt/status",NULL,200);
    call("POST","/api/mqtt/settings","{\"clear_password\":true}",200);assert(!mqtt.config.password[0]);
    r=request("GET","/api/mqtt/settings",NULL,NULL,NULL,200);v=cJSON_GetObjectItem(r,"config");
    assert(!cJSON_GetObjectItem(v,"password")->valuestring[0] && cJSON_IsFalse(cJSON_GetObjectItem(v,"password_set")));cJSON_Delete(r);
#ifdef _WIN32
    assert(!_putenv_s("GATEWAY_MQTT_PASSWORD","env-private-password"));
#else
    assert(!setenv("GATEWAY_MQTT_PASSWORD","env-private-password",1));
#endif
    call("POST","/api/mqtt/settings","{\"enabled\":false}",200);assert(!strcmp(mqtt.config.password,"env-private-password"));
    r=request("GET","/api/mqtt/settings",NULL,NULL,NULL,200);v=cJSON_GetObjectItem(r,"config");
    assert(!cJSON_GetObjectItem(v,"password")->valuestring[0] && cJSON_IsTrue(cJSON_GetObjectItem(v,"password_from_env")));cJSON_Delete(r);
    call("GET","/api/mqtt/status",NULL,200);
    call("POST","/api/mqtt/settings","{\"clear_password\":true}",400);
    after=read_text(path);assert(!strstr(after,"env-private-password"));free(after);
#ifdef _WIN32
    assert(!_putenv_s("GATEWAY_MQTT_PASSWORD",""));
#else
    assert(!unsetenv("GATEWAY_MQTT_PASSWORD"));
#endif
    before=read_text(path);write_text(path,"broken config");
    call("POST","/api/mqtt/settings","{\"enabled\":true}",500);after=read_text(path);
    assert(!strcmp(after,"broken config") && !mqtt.config.enabled);free(after);write_text(path,before);free(before);
    char original_path[512];strcpy(original_path,api.config_path);
    snprintf(api.config_path,sizeof(api.config_path),"%s/nonexistent/config.json",path);
    call("POST","/api/mqtt/settings","{\"enabled\":false}",500);assert(!mqtt.config.enabled);strcpy(api.config_path,original_path);
    call("POST","/api/mqtt/settings","{\"enabled\":true}",200);tick(300);
    assert(collections==2 && waveforms==2 && mqtt.ready);
    gateway_mqtt_api_stop(&api);gateway_mqtt_stop(&mqtt);mg_mgr_free(&manager);close_database();remove(path);
    puts("MQTT management: auth/probe isolation, config migration and JSON rejection, persistence/passwords/CORS, live reload, durable Sparkplug result/raw delivery PASS");
    return 0;
}
