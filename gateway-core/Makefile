CC ?= gcc

# 编译选项：保留工具链环境中的优化和ABI参数，再追加工程公共选项
CPPFLAGS += -I./include -DMG_ENABLE_HTTP=1 -DMG_ENABLE_LOG=0
CFLAGS ?= -g
CFLAGS += -Wall -Wextra
LDFLAGS ?=

# 链接选项：依赖的动态库最好单独拿出来，并且放在编译命令的最后面
LDLIBS += -lpthread -lsqlite3

# 注意这里：用空格把 src/uart_driver.c 和 src/db_manager.c 隔开
GATEWAY_SOURCES = src/main.c src/mongoose.c src/cJSON.c src/uart_driver.c src/db_manager.c src/gateway_log.c src/console_log.c src/mqtt_store.c src/gateway_mqtt.c src/gateway_mqtt_api.c src/sparkplug_b.c src/gateway_sparkplug.c
gateway-server: $(GATEWAY_SOURCES) $(wildcard include/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o gateway-server $(GATEWAY_SOURCES) $(LDLIBS)

clean:
	rm -f gateway-server

.PHONY: test
test-sparkplug:
	mkdir -p ../out/sparkplug-tests
	$(CC) $(CPPFLAGS) -std=c11 -UNDEBUG ../tests/test_sparkplug.c src/gateway_mqtt.c src/gateway_sparkplug.c src/sparkplug_b.c src/mqtt_store.c src/mongoose.c src/cJSON.c -lsqlite3 -lpthread -o ../out/sparkplug-tests/sparkplug-test
	../out/sparkplug-tests/sparkplug-test ../out/sparkplug-tests

.PHONY: test-sparkplug
test:
	mkdir -p ../out
	$(CC) -std=c11 -Wall -Wextra -Werror -Iinclude ../tests/test_single_upload.c -o ../out/zigbee-single-upload-test
	../out/zigbee-single-upload-test
	$(CC) -std=c11 -Wall -Wextra -Werror -Iinclude ../tests/test_vibration_v2.c -o ../out/zigbee-v2-test
	../out/zigbee-v2-test
	$(CC) -std=c11 -Iinclude ../tests/test_vibration_db.c -lsqlite3 -o ../out/zigbee-db-test
	../out/zigbee-db-test
	$(CC) -std=c11 -Iinclude ../tests/test_collection_history.c -lsqlite3 -o ../out/collection-history-test
	../out/collection-history-test
	$(CC) -std=c11 -Iinclude ../tests/test_vibration_gateway.c -lsqlite3 -o ../out/zigbee-gateway-test
	../out/zigbee-gateway-test
	$(CC) -std=c11 -Iinclude ../tests/test_upload_admission.c -o ../out/upload-admission-test
	../out/upload-admission-test
	$(CC) -std=c11 -Iinclude ../tests/test_upload_queue_gateway.c -lsqlite3 -o ../out/upload-queue-gateway-test
	../out/upload-queue-gateway-test
	$(CC) -std=c11 -Wall -Wextra -Werror -Iinclude ../tests/test_report_dedup.c -o ../out/report-dedup-test
	../out/report-dedup-test
	$(CC) -std=c11 -Iinclude ../tests/test_collection_ingest.c -lsqlite3 -o ../out/collection-ingest-test
	../out/collection-ingest-test
	$(CC) -std=c11 -Iinclude ../tests/test_gateway_logs.c -lsqlite3 -lpthread -o ../out/gateway-logs-test
	../out/gateway-logs-test
	$(CC) $(CPPFLAGS) -std=c11 ../tests/test_gateway_log_query.c src/mongoose.c -lpthread -o ../out/gateway-log-query-test
	../out/gateway-log-query-test
	$(CC) $(CPPFLAGS) -std=c11 ../tests/test_mqtt_store.c src/mqtt_store.c src/cJSON.c -lsqlite3 -o ../out/mqtt-store-test
	../out/mqtt-store-test ../out/mqtt-restart-test.db
	$(CC) $(CPPFLAGS) -std=gnu11 ../tests/test_mqtt_config.c src/gateway_mqtt.c src/gateway_sparkplug.c src/sparkplug_b.c src/mqtt_store.c src/mongoose.c src/cJSON.c -lsqlite3 -lpthread -o ../out/mqtt-config-test
	../out/mqtt-config-test ../out/mqtt-config-test.json
	$(CC) $(CPPFLAGS) -std=gnu11 ../tests/test_mqtt_client.c src/gateway_mqtt.c src/gateway_sparkplug.c src/sparkplug_b.c src/mqtt_store.c src/mongoose.c src/cJSON.c -lsqlite3 -lpthread -o ../out/mqtt-client-test
	../out/mqtt-client-test
	$(CC) $(CPPFLAGS) -std=gnu11 ../tests/test_mqtt_management.c src/gateway_mqtt.c src/gateway_mqtt_api.c src/gateway_sparkplug.c src/sparkplug_b.c src/mqtt_store.c src/mongoose.c src/cJSON.c -lsqlite3 -lpthread -o ../out/mqtt-management-test
	../out/mqtt-management-test ../out/mqtt-management-test.json
