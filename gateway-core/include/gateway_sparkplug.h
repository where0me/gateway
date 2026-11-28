#ifndef GATEWAY_SPARKPLUG_H
#define GATEWAY_SPARKPLUG_H
#include "gateway_mqtt.h"
int gateway_sparkplug_options(GatewayMqtt *m,struct mg_mqtt_opts *options);
void gateway_sparkplug_open(GatewayMqtt *m,uint64_t now);
void gateway_sparkplug_event(GatewayMqtt *m,int event,const struct mg_mqtt_message *message);
void gateway_sparkplug_poll(GatewayMqtt *m,uint64_t now);
void gateway_sparkplug_clear(GatewayMqtt *m);
#endif
