#ifndef VIBRATION_RX_H
#define VIBRATION_RX_H
#include "vibration_codec.h"
#include <stdlib.h>

typedef struct {
    uint8_t batch[V2_BATCH_LEN], block[V2_BLOCK_LEN], encoded[V2_RAW_BYTES];
    uint8_t *raw;
    uint8_t blocks_done[V2_MAX_BLOCKS];
    uint64_t fragments_received;
    unsigned block_index, encoded_len, fragments;
    int active, have_block, complete;
} V2Rx;
typedef struct {
    void *context;
    /* 1 = exact batch already durable, 0 = absent/incomplete, -1 = identity conflict/error */
    int (*stored)(void *, const uint8_t *batch);
    /* raw==NULL: feature-only metadata; raw!=NULL: complete raw, commit before returning 1. */
    int (*store)(void *, const uint8_t *batch, const uint8_t *raw);
    void (*progress)(void *,uint32_t batch,unsigned blocks,unsigned total);
} V2RxIO;
static inline void v2_rx_reset(V2Rx *r) {free(r->raw);memset(r,0,sizeof(*r));}
static inline unsigned v2_rx_done_count(const V2Rx *r) {
    unsigned n=0;for(unsigned i=0;i<V2_MAX_BLOCKS;++i)n+=r->blocks_done[i]!=0U;return n;
}
static inline int v2_rx_all_done(const V2Rx *r,unsigned total) {
    if(total>V2_MAX_BLOCKS)return 0;
    for(unsigned i=0;i<total;++i)if(!r->blocks_done[i])return 0;
    return 1;
}
/* Shared by automatic completion and explicit timeout-recovery queries. */
static inline int v2_rx_window(V2Rx *r,const V2RxIO *io,uint8_t reply[V2_ACK_LEN]) {
    uint64_t missing=v2_mask(r->fragments)&~r->fragments_received;
    unsigned status=V2_OK,block=r->block_index;
    uint32_t batch=v2_u32(r->batch+1);
    if(!r->active || !r->have_block)return 0;
    if(missing)status=V2_MISSING;
    else {
        unsigned points=v2_u16(r->block+7);
        uint8_t *raw=r->raw+(size_t)block*V2_RAW_BYTES;
        if(!v2_decode(r->block[9],r->encoded,r->encoded_len,points,raw,(size_t)points*6) ||
           v2_crc(raw,(size_t)points*6)!=v2_u32(r->block+12)) {
            r->fragments_received=0;status=V2_MISSING;missing=v2_mask(r->fragments);
        } else {
            r->blocks_done[block]=1U;r->have_block=0;
            if(io->progress)io->progress(io->context,batch,v2_rx_done_count(r),
                                          v2_blocks(v2_u32(r->batch+7)));
        }
    }
    v2_ack(reply,V2_KIND_WINDOW,batch,block,status,missing);return 1;
}
/* Only block/batch results generate ACKs, never ordinary individual fragments. */
static inline int v2_rx_receive(V2Rx *r,const V2RxIO *io,uint8_t command,
                                 const uint8_t *p,size_t n,uint8_t reply[V2_ACK_LEN]) {
    uint32_t batch=0;unsigned block=0,kind=0,status=V2_OK;uint64_t missing=0;
    if(command==V2_BATCH) {
        if(!v2_valid_batch(p,n))return 0;
        batch=v2_u32(p+1);kind=V2_KIND_BATCH;
        if(r->active && v2_u32(r->batch+1)==batch) {
            if(memcmp(r->batch,p,V2_BATCH_LEN))status=V2_REJECT;
            goto respond;
        }
        int durable=io->stored(io->context,p);
        if(durable<0){status=V2_REJECT;goto respond;}
        v2_rx_reset(r);
        if(!durable) {
            r->raw=(uint8_t *)calloc(v2_u32(p+7),6);
            if(!r->raw){status=V2_STORE_FAILED;goto respond;}
            if(!io->store(io->context,p,NULL)){v2_rx_reset(r);status=V2_STORE_FAILED;goto respond;}
        }
        memcpy(r->batch,p,V2_BATCH_LEN);r->active=1;r->complete=durable;
        if(durable) {
            unsigned total=v2_blocks(v2_u32(p+7));
            for(unsigned i=0;i<total;++i)r->blocks_done[i]=1U;
        }
        goto respond;
    }
    if(command==V2_BLOCK) {
        if(n!=V2_BLOCK_LEN || p[0]!=V2_VERSION)return 0;
        batch=v2_u32(p+1);block=v2_u16(p+5);kind=V2_KIND_BLOCK;
    } else if(command==V2_QUERY) {
        if(n!=8 || p[0]!=V2_VERSION || (p[1]!=V2_KIND_WINDOW && p[1]!=V2_KIND_FINAL))return 0;
        batch=v2_u32(p+2);block=v2_u16(p+6);kind=p[1];
    } else if(command==V2_DATA) {
        if(n<=7 || n>79)return 0;
        batch=v2_u32(p);block=v2_u16(p+4);
    } else return 0;
    if(!r->active || v2_u32(r->batch+1)!=batch) {
        if(!kind)return 0;
        status=V2_NEED_BATCH;goto respond;
    }
    unsigned blocks=v2_blocks(v2_u32(r->batch+7));
    if(kind==V2_KIND_FINAL) {
        if(block!=blocks){status=V2_REJECT;goto respond;}
        if(r->complete)goto respond;
        if(!v2_rx_all_done(r,blocks)) {status=V2_NEED_BATCH;goto respond;}
        size_t bytes=(size_t)v2_u32(r->batch+7)*6;
        if(v2_crc(r->raw,bytes)!=v2_u32(r->batch+13)) {
            memset(r->blocks_done,0,sizeof(r->blocks_done));
            r->have_block=0;status=V2_NEED_BATCH;goto respond;
        }
        if(!io->store(io->context,r->batch,r->raw)){status=V2_STORE_FAILED;goto respond;}
        free(r->raw);r->raw=NULL;r->complete=1;goto respond;
    }
    if(block>=blocks){if(!kind)return 0;status=V2_REJECT;goto respond;}
    if(kind==V2_KIND_BLOCK) {
        unsigned points=v2_block_points(v2_u32(r->batch+7),block),length=v2_u16(p+10);
        if(v2_u16(p+7)!=points || !length || length>points*6 ||
           (p[9]!=V2_RAW && p[9]!=V2_RICE) ||
           (p[9]==V2_RAW && length!=points*6) ||
           (p[9]==V2_RICE && length<9)) {status=V2_REJECT;goto respond;}
        if(r->blocks_done[block]){status=V2_BLOCK_DONE;goto respond;}
        if(r->have_block && r->block_index==block && !memcmp(r->block,p,V2_BLOCK_LEN))goto respond;
        memcpy(r->block,p,V2_BLOCK_LEN);r->block_index=block;r->encoded_len=length;
        r->fragments=v2_fragments(length);r->fragments_received=0;r->have_block=1;
        /* Only the first block needs an initial grant. */
        if(block!=0U)return 0;
        goto respond;
    }
    if(r->blocks_done[block]) {
        if(!kind)return 0;
        goto respond;
    }
    if(!r->have_block || r->block_index!=block) {
        if(!kind)return 0;
        status=V2_NEED_BLOCK;goto respond;
    }
    if(command==V2_DATA) {
        unsigned fragment=p[6],offset=fragment*V2_FRAGMENT_BYTES;
        if(fragment>=r->fragments)return 0;
        unsigned bytes=r->encoded_len-offset;
        if(bytes>V2_FRAGMENT_BYTES)bytes=V2_FRAGMENT_BYTES;
        if(n!=7+bytes)return 0;
        /* Same fragment overwrites safely; block CRC detects conflicting duplicates. */
        memcpy(r->encoded+offset,p+7,bytes);
        r->fragments_received|=UINT64_C(1)<<fragment;
        if(r->fragments_received==v2_mask(r->fragments))return v2_rx_window(r,io,reply);
        return 0;
    }
    if(kind==V2_KIND_WINDOW)return v2_rx_window(r,io,reply);
respond:
    v2_ack(reply,kind,batch,block,status,missing);return 1;
}
#endif
