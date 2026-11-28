#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define GATEWAY_DATABASE_PATH ":memory:"
#include "../gateway-core/src/db_manager.c"
#include "gateway_mqtt.h"
#include "sparkplug_b.h"
#include "cJSON.h"

/* Independent small wire inspector. Fixtures are also decoded by protobufjs
 * against the unmodified, official Eclipse Tahu .proto in the JS test. */
typedef struct {const uint8_t *p,*end;} Reader;
typedef struct {unsigned tag,wire;uint64_t integer;const uint8_t *data;size_t length;} Field;
static uint64_t var(Reader *r){uint64_t v=0;unsigned shift=0;for(;;){assert(r->p<r->end && shift<70);unsigned b=*r->p++;v|=(uint64_t)(b&127)<<shift;if(!(b&128))return v;shift+=7;}}
static int next(Reader *r,Field *f) {
    if(r->p==r->end)return 0;
    memset(f,0,sizeof(*f));uint64_t key=var(r);f->tag=(unsigned)(key>>3);f->wire=key&7;assert(f->tag);
    if(!f->wire){f->integer=var(r);return 1;}
    f->length=f->wire==2?(size_t)var(r):f->wire==1?8:f->wire==5?4:0;
    assert((f->wire==2 || f->length) && f->length<=(size_t)(r->end-r->p));f->data=r->p;r->p+=f->length;return 1;
}
typedef struct {char name[128];unsigned alias,type,is_null,historical;uint64_t integer;double number;const uint8_t *data;size_t length;} Metric;
static Metric metrics[100];static unsigned count,sequence,has_seq;
static void decode(const void *p,size_t n) {
    Reader r={p,(const uint8_t *)p+n};Field f;count=sequence=has_seq=0;
    while(next(&r,&f)) {
        if(f.tag==3){sequence=(unsigned)f.integer;has_seq=1;}
        if(f.tag!=2)continue;
        assert(count<100);Metric *m=&metrics[count++];memset(m,0,sizeof(*m));
        Reader sub={f.data,f.data+f.length};Field v;
        while(next(&sub,&v))switch(v.tag) {
            case 1:assert(v.length<sizeof(m->name));memcpy(m->name,v.data,v.length);break;
            case 2:m->alias=(unsigned)v.integer;break;
            case 4:m->type=(unsigned)v.integer;break;
            case 5:m->historical=(unsigned)v.integer;break;
            case 7:m->is_null=(unsigned)v.integer;break;
            case 10:case 11:case 14:m->integer=v.integer;break;
            case 13:{uint64_t bits=0;assert(v.length==8);for(unsigned i=0;i<8;i++)bits|=(uint64_t)v.data[i]<<(8*i);memcpy(&m->number,&bits,8);break;}
            case 15:case 16:m->data=v.data;m->length=v.length;break;
        }
    }
}
static Metric *alias(unsigned a){for(unsigned i=0;i<count;i++)if(metrics[i].alias==a)return &metrics[i];return NULL;}
static const char *mac="01:02:03:04:05:06:07:08",*offline_mac="11:12:13:14:15:16:17:18";
static uint8_t raw[40001*6];
static struct mg_connection *peer;
static unsigned connects,births,dbirths,deaths,collections,waves,expected_sequence,will_bdseq;
static int reject_subscription;
static const char *fixture_dir;
static void fixture(const char *name,const void *p,size_t n) {
    char path[768];snprintf(path,sizeof(path),"%s/%s.pb",fixture_dir,name);FILE *f=fopen(path,"wb");assert(f);
    assert(fwrite(p,1,n,f)==n && fclose(f)==0);
}
static Reader string16(Reader *r) {
    assert(r->end-r->p>=2);size_t n=((size_t)r->p[0]<<8)|r->p[1];r->p+=2;
    assert(n<=(size_t)(r->end-r->p));Reader s={r->p,r->p+n};r->p+=n;return s;
}
static void check_connect(const struct mg_mqtt_message *msg) {
    Reader r={(const uint8_t *)msg->dgram.buf+1,(const uint8_t *)msg->dgram.buf+msg->dgram.len};
    (void)var(&r);Reader protocol=string16(&r);assert(protocol.end-protocol.p==4 && !memcmp(protocol.p,"MQTT",4));
    assert(*r.p++==4);unsigned flags=*r.p++;assert((flags&0x3f)==0x0e);r.p+=2;(void)string16(&r);
    Reader t=string16(&r),payload=string16(&r);
    const char *will="spBv1.0/test-group/NDEATH/test-gw";assert(t.end-t.p==(int)strlen(will) && !memcmp(t.p,will,strlen(will)));
    decode(payload.p,(size_t)(payload.end-payload.p));assert(!has_seq && count==1 && !strcmp(metrics[0].name,"bdSeq") && metrics[0].type==SPB_INT64);
    will_bdseq=(unsigned)metrics[0].integer;assert(will_bdseq==(connects&255U));
    if(!reject_subscription)fixture("ndeath",payload.p,(size_t)(payload.end-payload.p));
}
static void broker(struct mg_connection *c,int event,void *data) {
    if(event==MG_EV_MQTT_CMD) {
        const struct mg_mqtt_message *msg=data;
        if(msg->cmd==MQTT_CMD_CONNECT){check_connect(msg);connects++;peer=c;const uint8_t ack[]={0x20,2,0,0};assert(mg_send(c,ack,sizeof(ack)));}
        if(msg->cmd==MQTT_CMD_SUBSCRIBE){const uint8_t ack[]={0x90,3,(uint8_t)(msg->id>>8),(uint8_t)msg->id,reject_subscription?0x80:1};assert(mg_send(c,ack,sizeof(ack)));}
        if(msg->cmd==MQTT_CMD_PINGREQ)mg_mqtt_pong(c);
    } else if(event==MG_EV_MQTT_MSG) {
        const struct mg_mqtt_message *msg=data;assert(msg->qos==0 && !((uint8_t)msg->dgram.buf[0]&1));
        char t[256];assert(msg->topic.len<sizeof(t));memcpy(t,msg->topic.buf,msg->topic.len);t[msg->topic.len]=0;
        decode(msg->data.buf,msg->data.len);assert(has_seq);
        if(strstr(t,"/NBIRTH/")) {
            assert(sequence==0 && alias(1) && alias(1)->type==SPB_BOOLEAN && !alias(1)->integer && alias(2)->integer==will_bdseq);
            assert(!strcmp(alias(1)->name,"Node Control/Rebirth"));births++;expected_sequence=1;
            fixture("nbirth",msg->data.buf,msg->data.len);return;
        }
        assert(sequence==expected_sequence);expected_sequence=(expected_sequence+1)&255U;
        if(strstr(t,"/DBIRTH/")) {
            assert(count==37 && alias(18) && alias(46)->is_null && !strcmp(alias(46)->name,"Waveform/Samples"));dbirths++;
            fixture("dbirth",msg->data.buf,msg->data.len);
        } else if(strstr(t,"/DDATA/")) {
            assert(alias(12) && (alias(12)->integer==42 || alias(12)->integer==43));
            uint64_t batch=alias(12)->integer;
            assert(alias(12)->historical==(batch==42));
            if(alias(46) && !alias(46)->is_null) {
                assert(alias(46)->length==sizeof(raw) && !memcmp(alias(46)->data,raw,sizeof(raw)));waves++;
                fixture(batch==42?"raw-historical":"raw-current",msg->data.buf,msg->data.len);
            } else {
                assert(alias(18) && alias(18)->number==(batch==42?26.31:27.5));
                assert(alias(19)->number==-1.5 && alias(14)->integer>0 && alias(13)->length);
                collections++;fixture(batch==42?"collection-historical":"collection-current",msg->data.buf,msg->data.len);
            }
        } else if(strstr(t,"/DDEATH/")){assert(count==0);deaths++;fixture("ddeath",msg->data.buf,msg->data.len);}
        else assert(strstr(t,"/NDATA/"));
    }
}
static void spin(struct mg_mgr *mgr,GatewayMqtt *m,unsigned ms) {
    uint64_t until=mg_millis()+ms;
    do{mg_mgr_poll(mgr,1);gateway_mqtt_poll(m,mg_millis());}while(mg_millis()<until);
}
static void command(int retained,int by_alias,const char *topic_name) {
    SpbBuffer b;spb_begin(&b,(uint64_t)time(NULL)*1000U,-1);
    SpbMetric metric={.name="Node Control/Rebirth",.alias=by_alias?1:0,.datatype=SPB_BOOLEAN,.integer=1};
    spb_add(&b,&metric,!by_alias);assert(spb_rebirth_request(b.data,b.length)==1);
    fixture(by_alias?"rebirth-alias":"rebirth-name",b.data,b.length);
    struct mg_mqtt_opts opts={0};opts.topic=mg_str(topic_name);opts.message=mg_str_n((char *)b.data,b.length);opts.retain=retained;
    mg_mqtt_pub(peer,&opts);spb_free(&b);
}
static void make_batch(const char *address,uint32_t batch,float temperature) {
    assert(record_collection_temp(address,batch,temperature)==1 && record_collection_mag(address,batch,-1.5f,2,3)==1);
    assert(record_collection_sound(address,batch,1,67)==1 && record_collection_sound(address,batch,0,68)==1);
    int32_t features[3][5]={{10,20,30,3000,7000000},{20,30,40,3001,7100000},{30,40,50,3002,7200000}};
    assert(save_vibration_batch(address,batch,26667,40001,raw,sizeof(raw),features)==0);
}
int main(int argc,char **argv) {
    assert(argc==2);fixture_dir=argv[1];mg_log_set(MG_LL_NONE);
    assert(init_database()==0 && insert_new_device(mac,"Test sensor")==0 && insert_new_device(offline_mac,"Offline sensor")==0);
    for(unsigned i=0;i<sizeof(raw);i++)raw[i]=(uint8_t)(i*17);
    assert(sqlite3_exec(g_db,"UPDATE devices SET status='online' WHERE mac='01:02:03:04:05:06:07:08';",NULL,NULL,NULL)==SQLITE_OK);
    GatewayMqttConfig config;gateway_mqtt_config_defaults(&config);config.enabled=1;
    strcpy(config.protocol,"sparkplug_b");strcpy(config.group_id,"test-group");strcpy(config.gateway_id,"test-gw");strcpy(config.client_id,"sparkplug-test");
    struct mg_mgr mgr;mg_mgr_init(&mgr);struct mg_connection *listener=mg_mqtt_listen(&mgr,"mqtt://127.0.0.1:0",broker,NULL);assert(listener);
    snprintf(config.url,sizeof(config.url),"mqtt://127.0.0.1:%u",(unsigned)mg_ntohs(listener->loc.port));
    GatewayMqtt m;assert(gateway_mqtt_init(&m,&mgr,g_db,&config)==0);
    make_batch(offline_mac,41,25);make_batch(mac,42,26.31f);make_batch(mac,43,27.5f);
    for(unsigned i=0;i<5;i++)assert(mqtt_store_reconcile(g_db,m.target,config.gateway_id,config.group_id,config.max_pending_bytes)>=0);
    spin(&mgr,&m,1600);assert(births==1 && dbirths==1 && collections==2 && waves==2 && mqtt_store_pending(g_db,m.target)==1);
    /* A retained command and a command for a different gateway do not rebirth. */
    command(1,0,"spBv1.0/test-group/NCMD/test-gw");command(0,0,"spBv1.0/test-group/NCMD/other");spin(&mgr,&m,200);assert(births==1);
    command(0,0,"spBv1.0/test-group/NCMD/test-gw");spin(&mgr,&m,1100);assert(births==2 && dbirths==2);
    command(0,1,"spBv1.0/test-group/NCMD/test-gw");spin(&mgr,&m,1100);assert(births==3 && dbirths==3 && waves==2);
    assert(sqlite3_exec(g_db,"UPDATE devices SET status='offline';",NULL,NULL,NULL)==SQLITE_OK);spin(&mgr,&m,1100);assert(deaths==1);
    assert(sqlite3_exec(g_db,"UPDATE devices SET status='online' WHERE mac='01:02:03:04:05:06:07:08';",NULL,NULL,NULL)==SQLITE_OK);spin(&mgr,&m,1100);assert(dbirths==4);
    /* Real reconnect: persisted bdSeq increments, NBIRTH resets seq to zero. */
    peer->is_closing=1;spin(&mgr,&m,1800);assert(connects==2 && births==4 && dbirths==5 && will_bdseq==1 && waves==2);
    char invalid_target[768];GatewayMqttConfig legacy=config;strcpy(legacy.protocol,"json");
    assert(!gateway_mqtt_config_valid(&legacy) && gateway_mqtt_prepare(&m,&legacy,invalid_target)==-1);
    /* Mandatory NCMD subscription is refused: never publish a birth or data. */
    reject_subscription=1;peer->is_closing=1;spin(&mgr,&m,1500);assert(births==4 && !strcmp(m.error_code,"subscription_denied"));
    gateway_mqtt_stop(&m);listener->is_closing=1;mg_mgr_free(&mgr);close_database();
    const uint8_t bad[]={0x12,0xff,0xff,0xff,0xff,0xff};assert(spb_rebirth_request(bad,sizeof(bad))==-1);
    puts("Sparkplug PASS: actual sockets, official wire tags, CONNECT will/bdSeq, subscribe before births, alias metrics, historical/null/raw data, rebirth, offline/backlog isolation, reconnect sequence and denied ACL");
    return 0;
}
