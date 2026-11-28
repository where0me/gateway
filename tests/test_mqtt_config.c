#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gateway_mqtt.h"
static void write_config(const char *path,const char *text) {
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(text,1,strlen(text),f)==strlen(text));assert(fclose(f)==0);
}
int main(int argc,char **argv) {
    assert(argc==2);GatewayMqttConfig config;
    write_config(argv[1],"{\"serial_path\":\"/dev/ttyACM0\"}");
    assert(gateway_mqtt_load_config(argv[1],&config)==0 && !config.enabled && config.keepalive_sec==60);
    write_config(argv[1],"{\"mqtt\":{\"enabled\":true,\"url\":\"mqtt://192.168.137.1:1883\",\"username\":\"gateway\",\"password\":\"test-only\"}}");
    assert(gateway_mqtt_load_config(argv[1],&config)==0 && config.enabled);
    assert(!strcmp(config.client_id,"gateway-gw001") && !strcmp(config.password,"test-only"));
    assert(!strcmp(config.protocol,"sparkplug_b") && !strcmp(config.group_id,"factory01"));
    write_config(argv[1],"{\"mqtt\":{\"enabled\":true,\"url\":\"mqtt://server:1883\",\"protocol\":\"json\",\"topic_prefix\":\"legacy/prefix\"}}");
    assert(gateway_mqtt_load_config(argv[1],&config)==0 && !strcmp(config.protocol,"sparkplug_b"));
    assert(!strcmp(config.group_id,"factory01"));
    write_config(argv[1],"{\"mqtt\":{\"enabled\":true,\"url\":\"mqtt://server:1883\",\"protocol\":\"sparkplug_b\",\"group_id\":\"line01\"}}");
    assert(gateway_mqtt_load_config(argv[1],&config)==0 && !strcmp(config.protocol,"sparkplug_b") && !strcmp(config.group_id,"line01"));
    const char *bad[]={
        "{\"mqtt\":{\"protocol\":\"unsupported\"}}",
        "{\"mqtt\":{\"protocol\":\"sparkplug_b\",\"group_id\":\"bad/group\"}}",
        "{\"mqtt\":{\"protocol\":\"sparkplug_b\",\"group_id\":\"\"}}",
        "{\"mqtt\":{\"enabled\":true,\"url\":\"mqtts://server:8883\"}}",
        "{\"mqtt\":{\"enabled\":true,\"url\":\"mqtt://user:secret@server:1883\"}}",
        "{\"mqtt\":{\"enabled\":true,\"url\":\"mqtt://server:1883/topic\"}}",
        "{\"mqtt\":{\"enabled\":1}}",
        "{\"mqtt\":{\"enabled\":false,\"ack_timeout_ms\":0}}",
        "{\"mqtt\":{\"enabled\":false,\"keepalive_sec\":2.5}}",
        "{\"mqtt\":{\"enabled\":false,\"max_pending_bytes\":-1}}",
        "not json", "{\"mqtt\":{\"enabled\":false}} trailing junk"
    };
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        write_config(argv[1],bad[i]);assert(gateway_mqtt_load_config(argv[1],&config)==-1 && !config.enabled);
    }
    remove(argv[1]);assert(gateway_mqtt_load_config(argv[1],&config)==-1 && !config.enabled);
    puts("MQTT configuration: backward-compatible disabled default, credentials, client identity, strict values and unsupported TLS fail-closed PASS");
    return 0;
}
