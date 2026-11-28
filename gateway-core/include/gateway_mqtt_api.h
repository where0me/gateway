#ifndef GATEWAY_MQTT_API_H
#define GATEWAY_MQTT_API_H
#include "gateway_mqtt.h"

typedef struct {
    struct mg_connection *connection;
    GatewayMqttConfig config;
    char id[48],state[16],code[48],message[192];
    uint64_t deadline;
} GatewayMqttProbe;
typedef struct {
    GatewayMqtt *mqtt;
    char config_path[512];
    int config_valid;
    unsigned sequence;
    GatewayMqttProbe probe;
} GatewayMqttApi;
void gateway_mqtt_api_init(GatewayMqttApi *api,GatewayMqtt *mqtt,const char *path);
/* Returns 1 when handled. Must precede the legacy wildcard CORS router. */
int gateway_mqtt_api_handle(GatewayMqttApi *api,struct mg_connection *connection,struct mg_http_message *message);
void gateway_mqtt_api_poll(GatewayMqttApi *api,uint64_t now);
void gateway_mqtt_api_stop(GatewayMqttApi *api);
#endif
