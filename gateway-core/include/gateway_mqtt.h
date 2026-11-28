#ifndef GATEWAY_MQTT_H
#define GATEWAY_MQTT_H
#include "mongoose.h"
#include "mqtt_store.h"
typedef struct {
    int enabled,client_id_auto;
    char protocol[16],group_id[64];
    char url[256],gateway_id[64],client_id[128],username[128],password[256];
    unsigned keepalive_sec,ack_timeout_ms,connect_timeout_ms;
    sqlite3_int64 max_pending_bytes;
} GatewayMqttConfig;
typedef struct GatewayMqtt {
    GatewayMqttConfig config;
    struct mg_mgr *mgr;
    struct mg_connection *connection;
    sqlite3 *db;
    char target[768];
    int ready,store_ready,stopping;
    MqttDelivery flight;
    uint64_t deadline,reconnect_at,reconcile_at,ping_at,pong_deadline,last_error,status_at;
    int last_unknown;
    unsigned backoff_ms;
    char error_code[48],error_message[192];
    int64_t error_at;
    struct GatewaySparkplug *sparkplug;
} GatewayMqtt;
struct cJSON;
void gateway_mqtt_config_defaults(GatewayMqttConfig *config);
int gateway_mqtt_config_parse(const struct cJSON *root,GatewayMqttConfig *out,int environment);
int gateway_mqtt_config_valid(const GatewayMqttConfig *config);
/* Missing mqtt section = disabled; credentials may be overridden with
 * GATEWAY_MQTT_USERNAME / GATEWAY_MQTT_PASSWORD. Never log the password. */
int gateway_mqtt_load_config(const char *path,GatewayMqttConfig *out);
int gateway_mqtt_init(GatewayMqtt *mqtt,struct mg_mgr *mgr,sqlite3 *db,const GatewayMqttConfig *config);
/* Call only from the Mongoose owner thread, including connection shutdown. */
void gateway_mqtt_poll(GatewayMqtt *mqtt,uint64_t now);
void gateway_mqtt_stop(GatewayMqtt *mqtt);
/* Prepare durable destination before saving, then apply on the owner thread.
 * Old connection callbacks are ignored; unacknowledged outbox rows survive. */
int gateway_mqtt_prepare(GatewayMqtt *mqtt,const GatewayMqttConfig *config,char target[768]);
void gateway_mqtt_apply(GatewayMqtt *mqtt,const GatewayMqttConfig *config,const char *target);
#endif
