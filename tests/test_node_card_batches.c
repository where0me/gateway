#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#define GATEWAY_DATABASE_PATH ":memory:"
#include "../gateway-core/src/db_manager.c"
#include "cJSON.h"

struct mg_connection { int unused; };
struct mg_http_message { const char *query; };
static int api_status;
static char *api_response;
static void mg_http_reply(struct mg_connection *c,int status,const char *headers,const char *format,...)
{
    (void)c;(void)headers;
    char text[65536]; va_list args; va_start(args,format);
    vsnprintf(text,sizeof(text),format,args);va_end(args);
    free(api_response);api_response=strdup(text);api_status=status;
}
static int mg_http_get_var(const char **query,const char *key,char *out,size_t size)
{
    const char *part=*query;size_t len=strlen(key);
    while(part && *part) {
        const char *end=strchr(part,'&');if(!end)end=part+strlen(part);
        if((size_t)(end-part)>len && !strncmp(part,key,len) && part[len]=='=') {
            size_t count=(size_t)(end-part)-len-1;
            if(count>=size)return -3;
            memcpy(out,part+len+1,count);out[count]=0;return (int)count;
        }
        part=*end?end+1:end;
    }
    return -1;
}
#define V2_GATEWAY_SLOTS 2
typedef struct {int active;unsigned done;} TestRx;
typedef struct {TestRx rx;uint32_t batch_id;char mac[32];int scalar_started;uint64_t last_fragment_ms;} V2GatewaySlot;
static V2GatewaySlot v2_gateway_slots[V2_GATEWAY_SLOTS];
static int g_vibration_mutex;
static void pthread_mutex_lock(int *mutex){(void)mutex;}
static void pthread_mutex_unlock(int *mutex){(void)mutex;}
static unsigned v2_rx_done_count(const TestRx *rx){return rx->done;}
void api_handle_vibration_latest(struct mg_connection *,struct mg_http_message *);
static cJSON *request(const char *query,int expected) {
    struct mg_http_message hm={query};api_handle_vibration_latest(NULL,&hm);
    assert(api_status==expected);cJSON *root=cJSON_Parse(api_response);assert(root);return root;
}
static double number(cJSON *root,const char *key){return cJSON_GetNumberValue(cJSON_GetObjectItem(root,key));}
int main(void) {
    int32_t ready[3][5]={{1000},{2000},{3000}},pending[3][5];
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<5;++j)pending[i][j]=INT32_MIN;
    int16_t samples[6]={1,2,3,4,5,6};
    assert(init_database()==0);
    assert(insert_new_device("one","one")==0);
    assert(update_device_collection_batch_state("one","capturing",0,100)==0);
    assert(sqlite3_exec(g_db,"UPDATE collection_capture_times SET captured_at=1000;",0,0,0)==SQLITE_OK);
    assert(save_vibration_batch("one",100,1000,2,samples,sizeof(samples),ready)==0);
    assert(upsert_collection_temp("one",100,24.5f)==0);
    assert(save_vibration_batch("one",3,1000,2,NULL,0,pending)==0);
    cJSON *root=request("mac=one&raw=0",200);
    assert(number(root,"batch_id")==3);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(root,"upload_started")));
    cJSON *last=cJSON_GetObjectItem(root,"last_complete");
    assert(number(last,"batch_id")==100);
    assert(number(last,"captured_at")==1000);
    assert(number(root,"captured_at")==0);
    assert(number(cJSON_GetObjectItem(last,"collection_values"),"temperature")==24.5);
    cJSON_Delete(root);
    // Explicit batch remains readable after newer descriptors/data arrive.
    root=request("mac=one&raw=1&batch=100",200);
    assert(number(root,"batch_id")==100);
    assert(number(root,"captured_at")==1000);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(root,"samples"))==2);
    assert(!cJSON_GetObjectItem(root,"last_complete"));cJSON_Delete(root);
    root=request("mac=other&batch=100",404);cJSON_Delete(root);
    root=request("mac=one&batch=99",404);cJSON_Delete(root);
    const char *bad[]={"mac=one&batch=0","mac=one&batch=-1","mac=one&batch=4294967296","mac=one&batch=3oops","mac=one&batch="};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i){root=request(bad[i],400);cJSON_Delete(root);}
    // RAM fragment progress also counts as a real transfer, even if no scalar
    // was enabled. Merely accepting the descriptor does not.
    v2_gateway_slots[0]=(V2GatewaySlot){.rx={.active=1,.done=1},.batch_id=3,.mac="one",.last_fragment_ms=1};
    root=request("mac=one",200);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(root,"upload_started")));
    assert(number(root,"raw_received_packets")==1);cJSON_Delete(root);
    // Raw alone must not become the last complete batch before features arrive.
    assert(save_vibration_batch("one",3,1000,2,samples,sizeof(samples),pending)==0);
    root=request("mac=one",200);
    assert(number(cJSON_GetObjectItem(root,"last_complete"),"batch_id")==100);cJSON_Delete(root);
    assert(save_vibration_batch("one",3,1000,2,NULL,0,ready)==0);
    root=request("mac=one",200);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(root,"raw_available")));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(root,"features_ready")));
    assert(!cJSON_GetObjectItem(root,"last_complete"));cJSON_Delete(root);
    free(api_response);close_database();
    puts("Node-card batch API: durable fallback, exact batch, node isolation, upload start and split completion PASS");
    return 0;
}
