#ifndef VIBRATION_WIRE_H
#define VIBRATION_WIRE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* V2, little endian. See docs/vibration-transfer-v2.md. */
#define V2_VERSION 2U
#define V2_BATCH 0x92U
#define V2_BLOCK 0x93U
#define V2_DATA 0x94U
#define V2_QUERY 0x95U
#define V2_ACK 0x0DU
#define V2_POINTS 512U
#define V2_RAW_BYTES (V2_POINTS * 6U)
#define V2_FRAGMENT_BYTES 72U
#define V2_MAX_SAMPLES 40001U
#define V2_MAX_BLOCKS 79U
#define V2_BATCH_LEN 77U
#define V2_BLOCK_LEN 16U
#define V2_ACK_LEN 17U
#define V2_KIND_BATCH 1U
#define V2_KIND_BLOCK 2U
#define V2_KIND_WINDOW 3U
#define V2_KIND_FINAL 4U
#define V2_KIND_FEATURES 5U
/* UART-only extension. A tagged window query/reply appends the query's ZCL
 * sequence after the original payload. The air protocol stays unchanged. */
#define V2_WINDOW_QUERY_LEN 8U
#define V2_WINDOW_QUERY_TAG_ENABLE 3U /* One-byte payload for command 0x15. */
#define V2_WINDOW_QUERY_COALESCED_EVENT 0x38U /* Coordinator diagnostic v1. */
static inline int v2_window_query_has_tag(uint8_t command,const uint8_t *p,size_t n)
{
    return command==V2_QUERY && n==V2_WINDOW_QUERY_LEN+1U &&
           p[0]==V2_VERSION && p[1]==V2_KIND_WINDOW;
}
#define V2_OK 0U
#define V2_MISSING 1U
#define V2_NEED_BATCH 2U
#define V2_REJECT 3U
#define V2_STORE_FAILED 4U
#define V2_NEED_BLOCK 5U
#define V2_BLOCK_DONE 6U
/* WAIT: ACK bytes 9..12 = retry-after milliseconds, 13..16 reserved.
   BLOCK/OK: bytes 9..12 = grant lifetime, 13..16 = minimum fragment spacing.
   Existing command IDs and envelope lengths remain unchanged. */
#define V2_WAIT 7U
#define V2_GRANT_MS 20000U
#define V2_GRANT_GUARD_MS 10000U
#define V2_QUEUE_RETRY_MS 500U
#define V2_RAW 0U
#define V2_RICE 1U

/* All 15 feature words set to INT32_MIN means features follow in 0x8C.
   Keep the V2 envelope unchanged so the coordinator remains a passthrough. */
static inline int v2_features_pending(const int32_t features[3][5]) {
    for(unsigned a=0;a<3;a++) for(unsigned f=0;f<5;f++)
        if(features[a][f]!=INT32_MIN) return 0;
    return 1;
}

static inline uint16_t v2_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t v2_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline void v2_w16(uint8_t *p, uint16_t n) { p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); }
static inline void v2_w32(uint8_t *p, uint32_t n) {
    p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); p[2]=(uint8_t)(n>>16); p[3]=(uint8_t)(n>>24);
}
static inline uint64_t v2_u64(const uint8_t *p) {
    return (uint64_t)v2_u32(p) | ((uint64_t)v2_u32(p+4)<<32);
}
static inline void v2_w64(uint8_t *p, uint64_t n) { v2_w32(p,(uint32_t)n); v2_w32(p+4,(uint32_t)(n>>32)); }
static inline uint32_t v2_crc_update(uint32_t crc, const uint8_t *p, size_t n) {
    while(n--) { crc^=*p++; for(unsigned b=0;b<8;b++) crc=(crc>>1)^((crc&1)?0xEDB88320U:0); }
    return crc;
}
static inline uint32_t v2_crc(const uint8_t *p, size_t n) { return v2_crc_update(UINT32_MAX,p,n)^UINT32_MAX; }
static inline unsigned v2_blocks(uint32_t n) { return (unsigned)((n+V2_POINTS-1)/V2_POINTS); }
static inline unsigned v2_fragments(unsigned n) { return (n+V2_FRAGMENT_BYTES-1)/V2_FRAGMENT_BYTES; }
static inline uint64_t v2_mask(unsigned n) { return n==64?UINT64_MAX:((UINT64_C(1)<<n)-1); }
static inline unsigned v2_block_points(uint32_t count, unsigned block) {
    uint32_t first=(uint32_t)block*V2_POINTS;
    if(first>=count) return 0;
    return count-first>V2_POINTS?V2_POINTS:(unsigned)(count-first);
}
static inline int v2_valid_batch(const uint8_t *p, size_t n) {
    uint32_t rate, count;
    if(n!=V2_BATCH_LEN || p[0]!=V2_VERSION || v2_u32(p+1)==0 || v2_u16(p+11)!=V2_POINTS) return 0;
    rate=v2_u16(p+5); count=v2_u32(p+7);
    return rate==26667 && count>0 && count<=V2_MAX_SAMPLES;
}
static inline void v2_ack(uint8_t *p, unsigned kind, uint32_t batch, unsigned block, unsigned status, uint64_t missing) {
    p[0]=V2_VERSION; p[1]=(uint8_t)kind; v2_w32(p+2,batch); v2_w16(p+6,(uint16_t)block);
    p[8]=(uint8_t)status; v2_w64(p+9,missing);
}
#endif
