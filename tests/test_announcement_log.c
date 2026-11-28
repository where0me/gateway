#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#define GATEWAY_DATABASE_PATH ":memory:"
#include "../gateway-core/src/db_manager.c"

static int g_uart_fd = -1, g_uart_mutex, g_ws_mutex;
static char g_ws_msg_buffer[512];
static unsigned announcements, notices, ws_events;
static void pthread_mutex_lock(int *m) { assert(!*m); *m = 1; }
static void pthread_mutex_unlock(int *m) { assert(*m); *m = 0; }
static void send_hex_frame_to_uart(int fd, unsigned cmd, unsigned len, const unsigned char *p)
{ (void)fd; (void)cmd; (void)len; (void)p; assert(0); }
static void gateway_log_event(const char *level, const char *source, const char *event,
    const char *mac, int address, long long batch, const char *format, ...)
{ (void)level; (void)source; (void)event; (void)mac; (void)address; (void)batch; (void)format; }
static void queue_ws_event_locked(void) { ws_events++; }
static void accept_verified_scan_node(const unsigned char *p, uint8_t lqi, int8_t rssi)
{
    (void)lqi; (void)rssi;
    char mac[24], address[5];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
        p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7]);
    snprintf(address, sizeof(address), "%04X", (unsigned)p[8] | ((unsigned)p[9] << 8));
    assert(insert_new_device(mac, "Unknown Device") == 0);
    assert(update_device_online(mac, address) == 0);
    announcements++;
}
static int capture_printf(const char *format, ...)
{
    char line[512];
    va_list ap; va_start(ap, format);
    int count = vsnprintf(line, sizeof(line), format, ap); va_end(ap);
    assert(strstr(line, "[静默重连]"));
    notices++;
    return count;
}
#define printf capture_printf
void handle_cmd_84_device_join(unsigned char *payload, unsigned char len);

int main(void)
{
    const char *mac = "01:02:03:04:05:06:07:08";
    unsigned char p[] = {1,2,3,4,5,6,7,8,0x34,0x12};
    assert(init_database() == 0);
    handle_cmd_84_device_join(p, sizeof(p));
    assert(announcements == 1 && notices == 0);
    handle_cmd_84_device_join(p, sizeof(p));
    handle_cmd_84_device_join(p, sizeof(p));
    assert(notices == 0 && ws_events == 2);
    assert(!device_announcement_is_reconnect(mac, 0x9999));
    assert(confirm_device_join(mac, 0x1234) == 1);
    handle_cmd_84_device_join(p, sizeof(p));
    assert(notices == 0); /* Late duplicate after first application proof. */
    assert(update_device_offline(mac) == 0);
    handle_cmd_84_device_join(p, sizeof(p));
    assert(notices == 1);
    handle_cmd_84_device_join(p, sizeof(p));
    assert(notices == 1);
    p[8] = 0x45;
    handle_cmd_84_device_join(p, sizeof(p));
    assert(notices == 2);
    handle_cmd_84_device_join(p, sizeof(p));
    assert(notices == 2);
    assert(delete_device(mac) == 0);
    handle_cmd_84_device_join(p, sizeof(p));
    handle_cmd_84_device_join(p, sizeof(p));
    assert(announcements == 2 && notices == 2); /* Deleted/re-added is new. */
    unsigned before = ws_events;
    handle_cmd_84_device_join(p, 9);
    assert(notices == 2 && ws_events == before);
    close_database();
    puts("Announcement logs: actual handler, fresh/repeated/confirmed/deleted nodes and reconnect dedup PASS");
    return 0;
}
