#ifndef GATEWAY_PROTOCOL_H
#define GATEWAY_PROTOCOL_H
#include <stdint.h>
#include <stdbool.h>
#include "control_sync.h"
/* UART-only authorization envelope and tagged AF result. */
#define GW_CMD_ADMISSION_GRANT       0x18U
#define GW_CAP_ADMISSION_GRANT       0x01U
#define GW_ADMISSION_RESULT_SIZE     10U
#define GW_ADMISSION_SUBMIT_FAILED   0U
#define GW_ADMISSION_AF_RESULT       1U
#define GW_MONITOR_CONTROL_CMD       0x0BU
#define GW_MONITOR_AUTHORIZE_OP      0x03U
/* ST/Linux wire protocol: FE CMD LEN PAYLOAD XOR(CMD,LEN,PAYLOAD). */
typedef struct { uint8_t cmd, len, data[255]; } GatewayFrame;
typedef struct { uint8_t state, index, bcc; GatewayFrame frame; } GatewayParser;
static inline bool Gateway_ParseByte(GatewayParser *p, uint8_t byte)
{
    switch (p->state) {
    case 0: if (byte == 0xFE) p->state = 1; break;
    case 1: p->frame.cmd = byte; p->bcc = byte; p->state = 2; break;
    case 2: p->frame.len = byte; p->index = 0; p->bcc ^= byte;
        p->state = byte ? 3 : 4; break;
    case 3: p->frame.data[p->index++] = byte; p->bcc ^= byte;
        if (p->index == p->frame.len) p->state = 4; break;
    case 4:
        if (byte == p->bcc) { p->state = 0; return true; }
        p->state = byte == 0xFE ? 1 : 0; break;
    default: p->state = 0; break;
    }
    return false;
}
static inline uint16_t Gateway_Encode(uint8_t cmd, const uint8_t *data,
                                    uint8_t len, uint8_t out[259])
{
    uint8_t bcc = cmd ^ len;
    out[0] = 0xFE; out[1] = cmd; out[2] = len;
    for (unsigned i = 0; i < len; ++i) { out[i + 3] = data[i]; bcc ^= data[i]; }
    out[len + 3] = bcc; return (uint16_t)len + 4;
}
static inline uint16_t Gateway_LE16(const uint8_t *p)
{ return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline void Gateway_Put16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
/* 0x8D: keep lifecycle states compatible with the legacy 8-byte report.
   Append genuine receiver telemetry only to idle (state=0) status reports. */
static inline uint8_t Gateway_MonitorReport(uint8_t out[11], uint16_t address,
                                          const uint8_t *payload, uint16_t size,
                                          uint8_t lqi, int8_t rssi)
{
    if (size != 6U && size != 7U) return 0;
    Gateway_Put16(out, address);
    for (unsigned i = 0; i < 6U; ++i) out[i + 2U] = payload[i];
    if (payload[1] != 0U) return 8U;
    out[8] = size == 7U ? payload[6] : 0U;
    out[9] = lqi; out[10] = (uint8_t)rssi;
    return 11U;
}
static inline uint8_t Gateway_ControlStatusReport(uint8_t *out, uint16_t address,
                                                const uint8_t *payload, unsigned size,
                                                uint8_t lqi, int8_t rssi)
{
    if (!control_status_valid(payload,size) && !control_final_status_valid(payload,size)) return 0;
    Gateway_Put16(out,address);
    for (unsigned i=0;i<size;i++) out[2+i]=payload[i];
    out[2+size]=lqi; out[3+size]=(uint8_t)rssi;
    return (uint8_t)(size+4U);
}
#endif
