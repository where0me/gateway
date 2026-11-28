#ifndef RADIO_DIAG_PROTOCOL_H
#define RADIO_DIAG_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* Metadata only, UART 0x9F v2. No application payload or security material. */
#define RADIO_DIAG_CMD 0x15U
#define RADIO_DIAG_LEN 40U
#define RADIO_DIAG_SUBMIT 1U
#define RADIO_DIAG_AF_RESULT 2U
#define RADIO_DIAG_MAC_RESULT 3U
#define RADIO_DIAG_POLL 4U
#define RADIO_DIAG_COVERAGE 5U
#define RADIO_DIAG_DROPPED 6U
#define RADIO_DIAG_HAS_REQ 1U
#define RADIO_DIAG_MAC_ACK 2U
#define RADIO_DIAG_INDIRECT 4U
#define RADIO_DIAG_AF_ID_VALID 8U
typedef struct {uint32_t batch;uint16_t block;uint8_t kind,result;uint32_t fingerprint,detail;} RadioAckMeta;
static inline uint16_t radio_u16(const uint8_t *p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static inline uint32_t radio_u32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static inline void radio_w16(uint8_t *p,uint16_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static inline void radio_w32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static inline RadioAckMeta radio_ack_meta(const uint8_t *p,size_t n)
{
    RadioAckMeta m={0,UINT16_MAX,0,0,0,0};
    if(p && n==17U && p[0]==2U){m.kind=p[1];m.batch=radio_u32(p+2);m.block=radio_u16(p+6);m.result=p[8];
        m.fingerprint=2166136261U;for(unsigned i=0;i<17U;i++){m.fingerprint^=p[i];m.fingerprint*=16777619U;}
        m.detail=radio_u32(p+9);
    }
    return m;
}
static inline void radio_diag_base(uint8_t p[RADIO_DIAG_LEN],uint8_t type,uint32_t tick,uint16_t address)
{
    memset(p,0,RADIO_DIAG_LEN);p[0]=2;p[1]=type;radio_w32(p+2,tick);radio_w16(p+6,address);
    p[8]=p[9]=UINT8_MAX;radio_w16(p+16,UINT16_MAX);radio_w32(p+28,UINT32_MAX);
}
#endif
