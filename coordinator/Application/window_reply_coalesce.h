#ifndef WINDOW_REPLY_COALESCE_H
#define WINDOW_REPLY_COALESCE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* UART-only extension matching gateway-core/include/vibration_wire.h. */
#define WINDOW_QUERY_TAG_ENABLE 3U
#define WINDOW_QUERY_COALESCED_EVENT 0x38U
#define WINDOW_REPLY_LEN 17U
#define WINDOW_REPLY_COALESCE_MS 3000U

typedef struct {
    bool valid;
    uint8_t query_seq;
    uint8_t ieee[8];
    uint8_t payload[WINDOW_REPLY_LEN];
} WindowReplyTag;

static inline bool window_query_is_taggable(uint8_t cmd,const uint8_t *p,size_t n)
{
    return cmd==0x95U && n==8U && p[0]==2U && p[1]==3U;
}

static inline bool window_reply_has_tag(const uint8_t *wire,size_t n)
{
    return n==2U+WINDOW_REPLY_LEN+1U && wire[2]==2U && wire[3]==3U;
}

static inline bool window_reply_matches(const WindowReplyTag *tag,const uint8_t ieee[8],
                                        uint8_t seq,const uint8_t *payload,uint32_t age_ms)
{
    return tag->valid && age_ms<WINDOW_REPLY_COALESCE_MS && tag->query_seq==seq &&
           memcmp(tag->ieee,ieee,sizeof(tag->ieee))==0 &&
           memcmp(tag->payload,payload,sizeof(tag->payload))==0;
}

#endif

