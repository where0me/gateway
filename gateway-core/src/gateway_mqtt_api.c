#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "gateway_mqtt_api.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

static const char *base_headers="Content-Type: application/json\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nVary: Origin\r\n";
static void reply(struct mg_connection *c,int status,const char *headers,cJSON *root) {
    char *json=cJSON_PrintUnformatted(root);cJSON_Delete(root);
    mg_http_reply(c,json?status:500,headers,"%s",json?json:"{\"status\":\"error\",\"msg\":\"内存不足\"}");free(json);
}
static void error_reply(struct mg_connection *c,int status,const char *headers,const char *message) {
    cJSON *root=cJSON_CreateObject();cJSON_AddStringToObject(root,"status","error");cJSON_AddStringToObject(root,"msg",message);reply(c,status,headers,root);
}
static int equal_host(struct mg_str a,struct mg_str b) {
    return (a.len==b.len && !mg_strcasecmp(a,b)) ||
        ((mg_strcmp(a,mg_str("localhost"))==0 || mg_strcmp(a,mg_str("127.0.0.1"))==0 || mg_strcmp(a,mg_str("[::1]"))==0) &&
         (mg_strcmp(b,mg_str("localhost"))==0 || mg_strcmp(b,mg_str("127.0.0.1"))==0 || mg_strcmp(b,mg_str("[::1]"))==0));
}
/* Keep the trusted-LAN UI model, but do not allow arbitrary websites to read
 * MQTT settings or submit credentials via the existing wildcard CORS policy. */
static int response_headers(struct mg_http_message *hm,char *out,size_t size) {
    struct mg_str *origin=mg_http_get_header(hm,"Origin"),*host=mg_http_get_header(hm,"Host");
    if(!origin) {snprintf(out,size,"%s",base_headers);return 1;}
    if(!host || origin->len>=256 || host->len>=256)return 0;
    char origin_url[256],host_url[264];
    snprintf(origin_url,sizeof(origin_url),"%.*s",(int)origin->len,origin->buf);
    snprintf(host_url,sizeof(host_url),"http://%.*s",(int)host->len,host->buf);
    if((strncmp(origin_url,"http://",7) && strncmp(origin_url,"https://",8)) ||
       strchr(origin_url,'\r') || strchr(origin_url,'\n') || !equal_host(mg_url_host(origin_url),mg_url_host(host_url)))return 0;
    snprintf(out,size,"%sAccess-Control-Allow-Origin: %s\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\n",base_headers,origin_url);
    return 1;
}
static cJSON *read_root(const char *path,int create) {
    FILE *f=fopen(path,"rb");if(!f)return create && errno==ENOENT?cJSON_CreateObject():NULL;
    char *buffer=malloc(65537);if(!buffer){fclose(f);return NULL;}
    size_t n=fread(buffer,1,65536,f);int bad=ferror(f) || fgetc(f)!=EOF;fclose(f);buffer[n]=0;
    cJSON *root=bad?NULL:cJSON_ParseWithLengthOpts(buffer,n+1,NULL,1);free(buffer);
    if(!cJSON_IsObject(root)){cJSON_Delete(root);return NULL;}return root;
}
static int write_root(const char *path,cJSON *root) {
    char *json=cJSON_Print(root);if(!json)return -1;
    if(strlen(json)>65536){free(json);return -1;}
    char temporary[560];snprintf(temporary,sizeof(temporary),"%s.mqtt-XXXXXX",path);
#ifdef _WIN32
    int fd=-1;
    if(_mktemp_s(temporary,sizeof(temporary))==0)fd=_open(temporary,_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY,_S_IREAD|_S_IWRITE);
    FILE *f=fd>=0?_fdopen(fd,"wb"):NULL;
#else
    int fd=mkstemp(temporary);FILE *f=fd>=0?fdopen(fd,"wb"):NULL;
#endif
    if(!f) {
        if(fd>=0) {
#ifdef _WIN32
            _close(fd);
#else
            close(fd);
#endif
            remove(temporary);
        }
        free(json);return -1;
    }
    int bad=fwrite(json,1,strlen(json),f)!=strlen(json);free(json);
    if(fflush(f)!=0)bad=1;
#ifdef _WIN32
    if(_commit(fd)!=0)bad=1;
#else
    if(fsync(fd)!=0)bad=1;
#endif
    if(fclose(f)!=0)bad=1;
    if(!bad) {
#ifdef _WIN32
        if(!MoveFileExA(temporary,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))bad=1;
#else
        if(rename(temporary,path)!=0)bad=1;
#endif
    }
    if (bad) {
        remove(temporary);
    }
    return bad ? -1 : 0;
}
static int set_value(cJSON *object,const char *key,cJSON *value) {
    if(!value)return 0;
    cJSON_DeleteItemFromObjectCaseSensitive(object,key);
    if(!cJSON_AddItemToObject(object,key,value)){cJSON_Delete(value);return 0;}return 1;
}
static int allowed_key(const char *key) {
    const char *keys[]={"enabled","url","protocol","group_id","gateway_id","client_id","username","password","clear_password","keepalive_sec","ack_timeout_ms"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (!strcmp(key, keys[i])) {
            return 1;
        }
    }
    return 0;
}
static cJSON *request_json(const struct mg_http_message *hm) {
    if(!hm->body.len || hm->body.len>8192 || memchr(hm->body.buf,0,hm->body.len))return NULL;
    char *buffer=malloc(hm->body.len+1);if(!buffer)return NULL;
    memcpy(buffer,hm->body.buf,hm->body.len);buffer[hm->body.len]=0;
    /* cJSON strings are NUL-terminated; reject embedded-NUL escape ambiguity. */
    cJSON *root=strstr(buffer,"\\u0000")?NULL:cJSON_ParseWithLengthOpts(buffer,hm->body.len+1,NULL,1);free(buffer);
    if(!cJSON_IsObject(root)){cJSON_Delete(root);return NULL;}
    for(const cJSON *v=root->child;v;v=v->next) {
        if(!v->string || !allowed_key(v->string)){cJSON_Delete(root);return NULL;}
        for(const cJSON *p=v->next;p;p=p->next)if(p->string && !strcmp(v->string,p->string)){cJSON_Delete(root);return NULL;}
    }
    return root;
}
static int candidate(GatewayMqttApi *api,const cJSON *request,cJSON **file_root,GatewayMqttConfig *config,const char **message) {
    const cJSON *protocol=cJSON_GetObjectItemCaseSensitive(request,"protocol");
    if(protocol && (!cJSON_IsString(protocol) || strcmp(protocol->valuestring,"sparkplug_b"))) {
        *message="仅支持 Sparkplug B 上传协议，请刷新设置页面";return 400;
    }
    cJSON *root=read_root(api->config_path,1);if(!root){*message="无法读取配置文件，未覆盖现有文件";return 500;}
    cJSON *mqtt=cJSON_GetObjectItemCaseSensitive(root,"mqtt");
    if(!mqtt) {mqtt=cJSON_AddObjectToObject(root,"mqtt");if(!mqtt)goto oom;}
    if(!cJSON_IsObject(mqtt)){*message="配置文件的 MQTT 部分无效，请先修复";cJSON_Delete(root);return 500;}
    const cJSON *pass=cJSON_GetObjectItemCaseSensitive(request,"password"),*clear=cJSON_GetObjectItemCaseSensitive(request,"clear_password");
    const char *env_user=getenv("GATEWAY_MQTT_USERNAME"),*env_pass=getenv("GATEWAY_MQTT_PASSWORD");
    if((pass && !cJSON_IsString(pass)) || (clear && !cJSON_IsBool(clear)) || (cJSON_IsTrue(clear) && pass && pass->valuestring[0]))goto invalid;
    if(env_pass && ((pass && pass->valuestring[0]) || cJSON_IsTrue(clear))) {
        *message="密码由环境变量管理，不能在此修改";cJSON_Delete(root);return 400;
    }
    for(const cJSON *v=request->child;v;v=v->next) {
        const char *key=v->string;
        if(!strcmp(key,"password") || !strcmp(key,"clear_password"))continue;
        if(!strcmp(key,"username") && env_user) {
            if(!cJSON_IsString(v) || strcmp(v->valuestring,env_user)){*message="用户名由环境变量管理，不能在此修改";cJSON_Delete(root);return 400;}
            continue;
        }
        if(!set_value(mqtt,key,cJSON_Duplicate(v,1)))goto oom;
    }
    /* Blank/missing password means keep; explicit clear is a separate action. */
    if(cJSON_IsTrue(clear)) {if(!set_value(mqtt,"password",cJSON_CreateString("")))goto oom;}
    else if(pass && pass->valuestring[0]) {if(!set_value(mqtt,"password",cJSON_Duplicate(pass,1)))goto oom;}
    if(gateway_mqtt_config_parse(root,config,1)!=0)goto invalid;
    /* Save migrated defaults as well as the user's edits; never persist the
     * retired wire format or its topic prefix after a successful save. */
    if(!set_value(mqtt,"protocol",cJSON_CreateString("sparkplug_b")) ||
       !set_value(mqtt,"group_id",cJSON_CreateString(config->group_id)))goto oom;
    cJSON_DeleteItemFromObjectCaseSensitive(mqtt,"topic_prefix");
    *file_root=root;return 0;
invalid:
    *message="MQTT 配置无效，请检查地址、端口、标识和时间范围；暂不支持 TLS";cJSON_Delete(root);return 400;
oom:
    *message="内存不足，配置未保存";cJSON_Delete(root);return 500;
}
static cJSON *runtime_json(GatewayMqtt *m) {
    cJSON *root=cJSON_CreateObject();MqttStoreStats stats;
    int valid=m->store_ready && mqtt_store_backlog_stats(m->db,m->target,
        m->config.gateway_id,m->config.group_id,&stats)==0;
    const char *state=!m->store_ready?"error":!m->config.enabled?"disabled":m->ready?"connected":
        m->connection && !m->connection->is_closing?"connecting":m->error_code[0]?"error":"disconnected";
    cJSON_AddStringToObject(root,"state",state);
    cJSON_AddStringToObject(root,"delivery_semantics","socket_sent");
    cJSON_AddStringToObject(root,"error_code",m->error_code);cJSON_AddStringToObject(root,"error_message",m->error_message);
    cJSON_AddNumberToObject(root,"error_at",(double)m->error_at);cJSON_AddBoolToObject(root,"queue_available",valid);
    if(valid) {
        cJSON_AddNumberToObject(root,"pending_batches",(double)stats.pending_batches);
        cJSON_AddNumberToObject(root,"pending_bytes",(double)stats.pending_bytes);
        cJSON_AddNumberToObject(root,"other_pending_batches",(double)stats.other_pending_batches);
        if(stats.last_confirmed_at)cJSON_AddNumberToObject(root,"last_confirmed_at",(double)stats.last_confirmed_at);
        else cJSON_AddNullToObject(root,"last_confirmed_at");
    } else {
        cJSON_AddNullToObject(root,"pending_batches");cJSON_AddNullToObject(root,"pending_bytes");
        cJSON_AddNullToObject(root,"other_pending_batches");cJSON_AddNullToObject(root,"last_confirmed_at");
    }
    return root;
}
static void settings_reply(GatewayMqttApi *api,struct mg_connection *c,const char *headers,int include_config) {
    GatewayMqttConfig *cfg=&api->mqtt->config;cJSON *root=cJSON_CreateObject();
    cJSON_AddStringToObject(root,"status","success");cJSON_AddBoolToObject(root,"config_valid",api->config_valid);
    if(include_config) {
        cJSON *v=cJSON_AddObjectToObject(root,"config");
        cJSON_AddBoolToObject(v,"enabled",cfg->enabled);cJSON_AddStringToObject(v,"url",cfg->url);
        cJSON_AddStringToObject(v,"protocol","sparkplug_b");
        cJSON_AddStringToObject(v,"group_id",cfg->group_id);
        cJSON_AddStringToObject(v,"gateway_id",cfg->gateway_id);
        cJSON_AddStringToObject(v,"client_id",cfg->client_id_auto?"":cfg->client_id);
        cJSON_AddStringToObject(v,"effective_client_id",cfg->client_id);cJSON_AddStringToObject(v,"username",cfg->username);
        cJSON_AddBoolToObject(v,"password_set",cfg->password[0]!=0);
        /* Only settings responses expose the editable, file-managed credential.
         * Environment-managed secrets remain server-side. Responses are no-store. */
        cJSON_AddStringToObject(v,"password",getenv("GATEWAY_MQTT_PASSWORD")?"":cfg->password);
        cJSON_AddBoolToObject(v,"username_from_env",getenv("GATEWAY_MQTT_USERNAME")!=NULL);
        cJSON_AddBoolToObject(v,"password_from_env",getenv("GATEWAY_MQTT_PASSWORD")!=NULL);
        cJSON_AddNumberToObject(v,"keepalive_sec",cfg->keepalive_sec);cJSON_AddNumberToObject(v,"ack_timeout_ms",cfg->ack_timeout_ms);
        cJSON_AddNumberToObject(v,"connect_timeout_ms",cfg->connect_timeout_ms);
    }
    cJSON_AddItemToObject(root,"runtime",runtime_json(api->mqtt));reply(c,200,headers,root);
}
static void probe_finish(GatewayMqttProbe *p,int success,const char *code,const char *message) {
    snprintf(p->state,sizeof(p->state),"%s",success?"success":"failed");
    snprintf(p->code,sizeof(p->code),"%s",code);snprintf(p->message,sizeof(p->message),"%s",message);
    if(p->connection)p->connection->is_closing=1;
    p->connection=NULL;memset(p->config.password,0,sizeof(p->config.password));
}
static void probe_event(struct mg_connection *c,int event,void *data) {
    GatewayMqttProbe *p=c->fn_data;if(p->connection!=c)return;
    if(event==MG_EV_MQTT_OPEN) {
        unsigned status=*(const uint8_t *)data;
        probe_finish(p,status==0,status==0?"":status==4?"bad_credentials":status==5?"not_authorized":"connection_rejected",
            status==0?"连接和账号认证成功；此测试不验证主题发布权限":status==4?"用户名或密码错误":status==5?"服务器拒绝访问，请检查账号权限":"服务器拒绝 MQTT 连接");
    } else if(event==MG_EV_ERROR)probe_finish(p,0,"transport_error","无法连接服务器，请检查地址、端口和网络");
    else if(event==MG_EV_CLOSE)probe_finish(p,0,"disconnected","服务器在认证完成前断开了连接");
}
static void probe_reply(GatewayMqttProbe *p,struct mg_connection *c,int status,const char *headers) {
    cJSON *root=cJSON_CreateObject();cJSON_AddStringToObject(root,"status","success");
    cJSON_AddStringToObject(root,"id",p->id);cJSON_AddStringToObject(root,"state",p->state);
    cJSON_AddStringToObject(root,"code",p->code);cJSON_AddStringToObject(root,"message",p->message);reply(c,status,headers,root);
}
void gateway_mqtt_api_init(GatewayMqttApi *api,GatewayMqtt *mqtt,const char *path) {
    memset(api,0,sizeof(*api));api->mqtt=mqtt;snprintf(api->config_path,sizeof(api->config_path),"%s",path);
    GatewayMqttConfig unused;api->config_valid=gateway_mqtt_load_config(path,&unused)==0;
}
int gateway_mqtt_api_handle(GatewayMqttApi *api,struct mg_connection *c,struct mg_http_message *hm) {
    int settings=mg_strcmp(hm->uri,mg_str("/api/mqtt/settings"))==0;
    int status=mg_strcmp(hm->uri,mg_str("/api/mqtt/status"))==0;
    int test=mg_strcmp(hm->uri,mg_str("/api/mqtt/test"))==0;
    if(!settings && !status && !test)return 0;
    char headers[768];
    if(!response_headers(hm,headers,sizeof(headers))) {error_reply(c,403,base_headers,"不允许此网页来源访问 MQTT 设置");return 1;}
    if(mg_strcmp(hm->method,mg_str("OPTIONS"))==0) {mg_http_reply(c,204,headers,"");return 1;}
    if(mg_strcmp(hm->method,mg_str("GET"))==0) {
        if(!test)settings_reply(api,c,headers,settings);
        else {
            char id[48];int n=mg_http_get_var(&hm->query,"id",id,sizeof(id));
            if(n<=0 || strcmp(id,api->probe.id))error_reply(c,409,headers,"连接测试已过期或不存在，请重新测试");
            else probe_reply(&api->probe,c,200,headers);
        }
        return 1;
    }
    if(status || mg_strcmp(hm->method,mg_str("POST"))!=0){error_reply(c,405,headers,"请求方法不支持");return 1;}
    struct mg_str *type=mg_http_get_header(hm,"Content-Type");
    if(!type || type->len<16 || memcmp(type->buf,"application/json",16) ||
       (type->len>16 && type->buf[16]!=';' && type->buf[16]!=' ')) {error_reply(c,415,headers,"请使用 application/json");return 1;}
    cJSON *request=request_json(hm);if(!request){error_reply(c,400,headers,"JSON 无效、字段重复或包含不支持的配置项");return 1;}
    cJSON *file_root=NULL;GatewayMqttConfig config;const char *message="配置无效";
    int result=candidate(api,request,&file_root,&config,&message);cJSON_Delete(request);
    if(result) {error_reply(c,result,headers,message);return 1;}
    if(test) {
        cJSON_Delete(file_root);
        if(api->probe.connection){error_reply(c,409,headers,"已有连接测试正在进行，请稍后重试");return 1;}
        config.enabled=1;
        if(!gateway_mqtt_config_valid(&config)){error_reply(c,400,headers,"请填写有效的服务器地址");return 1;}
        GatewayMqttProbe *p=&api->probe;memset(p,0,sizeof(*p));p->config=config;
        snprintf(p->id,sizeof(p->id),"%llu-%u",(unsigned long long)mg_millis(),++api->sequence);
        snprintf(p->config.client_id,sizeof(p->config.client_id),"gateway-test-%s",p->id);
        strcpy(p->state,"testing");p->deadline=mg_millis()+config.connect_timeout_ms;
        struct mg_mqtt_opts opts={0};opts.version=4;opts.clean=true;opts.keepalive=15;
        opts.client_id=mg_str(p->config.client_id);opts.user=mg_str(p->config.username);opts.pass=mg_str(p->config.password);
        p->connection=mg_mqtt_connect(api->mqtt->mgr,p->config.url,&opts,probe_event,p);
        if(!p->connection)probe_finish(p,0,"connect_failed","无法创建测试连接");
        probe_reply(p,c,202,headers);return 1;
    }
    char target[768];
    if(gateway_mqtt_prepare(api->mqtt,&config,target)!=0) {cJSON_Delete(file_root);error_reply(c,503,headers,"上传队列不可用，未保存配置");return 1;}
    result=write_root(api->config_path,file_root);cJSON_Delete(file_root);
    if(result){error_reply(c,500,headers,"配置文件保存失败，运行配置未改变");return 1;}
    if(api->probe.connection)probe_finish(&api->probe,0,"cancelled","配置已更新，请重新测试");
    gateway_mqtt_apply(api->mqtt,&config,target);api->config_valid=1;
    settings_reply(api,c,headers,1);return 1;
}
void gateway_mqtt_api_poll(GatewayMqttApi *api,uint64_t now) {
    if(api->probe.connection && now>=api->probe.deadline)probe_finish(&api->probe,0,"connect_timeout","连接测试超时，请检查服务器和网络");
}
void gateway_mqtt_api_stop(GatewayMqttApi *api) {
    if(api->probe.connection)probe_finish(&api->probe,0,"cancelled","网关正在停止");
}
