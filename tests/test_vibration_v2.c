#include <stdio.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include "vibration_tx.h"
#include "vibration_rx.h"

static uint32_t random_state=3;
static uint32_t rnd(void){random_state=random_state*1664525U+1013904223U;return random_state;}
static uint8_t raw[V2_MAX_SAMPLES*6],encoded[V2_RAW_BYTES],decoded[V2_RAW_BYTES];
static void make_raw(unsigned points,int mode){
    for(unsigned i=0;i<points;i++)for(unsigned a=0;a<3;a++){
        int16_t n=mode==0?123:mode==1?(int16_t)((i%80)*8+a*311-300):
            mode==2?(int16_t)rnd():(i&1?INT16_MIN:INT16_MAX);
        v2_w16(raw+(i*3+a)*2,(uint16_t)n);
    }
}
static void codec_tests(void){
    assert(v2_crc((const uint8_t*)"123456789",9)==0xCBF43926U);
    for(unsigned points=1;points<=512;points+=1)for(int mode=0;mode<4;mode++){
        make_raw(points,mode);uint8_t codec;
        size_t n=v2_encode(raw,points,encoded,sizeof(encoded),&codec);
        assert(n>0 && n<=points*6);
        assert(v2_decode(codec,encoded,n,points,decoded,sizeof(decoded)));
        assert(!memcmp(raw,decoded,points*6));
        if(n>1)assert(!v2_decode(codec,encoded,n-1,points,decoded,sizeof(decoded)));
    }
    for(unsigned trial=0;trial<10000;trial++){
        unsigned n=1+rnd()%V2_RAW_BYTES,points=1+rnd()%512;
        for(unsigned i=0;i<n;i++)encoded[i]=(uint8_t)rnd();
        (void)v2_decode(V2_RICE,encoded,n,points,decoded,sizeof(decoded));
    }
    puts("codec: all sizes 1..512, constant/ramp/noise/extremes, truncation, fuzz OK");
}
typedef struct {uint32_t at;uint8_t command,data[79];size_t n;int used;} Event;
typedef struct {
    V2Tx tx;V2Rx rx;uint8_t descriptor[V2_BATCH_LEN],saved[V2_MAX_SAMPLES*6];
    Event events[512];uint32_t now;
    unsigned modes,ack_dropped[5],data_seen[V2_MAX_BLOCKS][43],stores,db_failures,sends,missing_acks;
    int durable,restarted,all_lost,corrupted;
} Simulation;
static Simulation sim;
static void enqueue(Simulation *s,uint8_t cmd,const uint8_t *p,size_t n,uint32_t delay){
    for(unsigned i=0;i<512;i++)if(!s->events[i].used){
        Event *e=&s->events[i];e->used=1;e->command=cmd;e->at=s->now+delay;e->n=n;memcpy(e->data,p,n);return;
    }
    assert(!"event overflow");
}
static int read_raw(void *arg,uint32_t batch,uint32_t first,unsigned points,uint8_t *out){
    Simulation *s=arg;assert(batch==v2_u32(s->descriptor+1));assert(first+points<=v2_u32(s->descriptor+7));
    memcpy(out,raw+first*6,points*6);return 1;
}
static int send_packet(void *arg,uint8_t command,const uint8_t *p,size_t n){
    Simulation *s=arg;s->sends++;
    if(s->modes&1){
        if(s->sends%17==0)return 0; /* transient APS queue backpressure */
        if(command==V2_DATA){
            unsigned block=v2_u16(p+4),fragment=p[6];
            unsigned seen=s->data_seen[block][fragment]++;
            if(fragment==2 && !seen)return 1; /* lose one fragment in every window */
        }
    }
    enqueue(s,command,p,n,24+(s->modes&1?rnd()%4*12:0));
    if ((s->modes&16) && !s->corrupted && command==V2_DATA)
    {
        for (unsigned i=0;i<512;i++) if(s->events[i].used && s->events[i].command==V2_DATA) {
            s->events[i].data[7]^=0x80;s->corrupted=1;break;
        }
    }
    if((s->modes&1) && command==V2_DATA && p[6]==4)enqueue(s,command,p,n,36);
    return 1;
}
static int is_stored(void *arg,const uint8_t *p){
    Simulation *s=arg;
    if(memcmp(p,s->descriptor,V2_BATCH_LEN))return -1;
    return s->durable;
}
static int store_raw(void *arg,const uint8_t *p,const uint8_t *data){
    Simulation *s=arg;assert(!memcmp(p,s->descriptor,V2_BATCH_LEN));
    if(!data)return 1;
    if(s->db_failures){s->db_failures--;return 0;}
    assert(!memcmp(raw,data,v2_u32(p+7)*6));
    memcpy(s->saved,data,v2_u32(p+7)*6);s->durable=1;s->stores++;return 1;
}
static void delivery(Simulation *s){
    V2RxIO io={s,is_stored,store_raw,NULL};
    for(unsigned i=0;i<512;i++){
        Event e=s->events[i];if(!e.used || !v2_due(s->now,e.at))continue;
        s->events[i].used=0;
        if(e.command==V2_ACK){v2_tx_receive(&s->tx,e.data,e.n,s->now);continue;}
        if((s->modes&2) && !s->restarted && e.command==V2_DATA && v2_u16(e.data+4)==2){
            v2_rx_reset(&s->rx);s->restarted=1;
        }
        uint8_t ack[V2_ACK_LEN];
        if(v2_rx_receive(&s->rx,&io,e.command,e.data,
                        e.command==V2_BLOCK?V2_BLOCK_LEN:e.n,ack)){
            if(ack[8]==V2_MISSING)s->missing_acks++;
            if(s->all_lost)continue;
            if((s->modes&1) && s->ack_dropped[ack[1]]++==0)continue;
            if((s->modes&4) && ack[1]==V2_KIND_FINAL && s->durable && !s->restarted){
                v2_rx_reset(&s->rx);s->restarted=1;continue; /* commit succeeded, final ACK lost, reboot */
            }
            enqueue(s,V2_ACK,ack,sizeof(ack),24);
        }
    }
}
static void transfer_test(unsigned points,int mode,unsigned faults,int lost){
    memset(&sim,0,sizeof(sim));make_raw(points,mode);
    Simulation *s=&sim;
    uint8_t *p=s->descriptor;p[0]=V2_VERSION;v2_w32(p+1,0x87654321);
    v2_w16(p+5,26667);v2_w32(p+7,points);v2_w16(p+11,V2_POINTS);v2_w32(p+13,v2_crc(raw,points*6));
    if(faults&32)p[13]^=1; /* valid blocks, wrong whole-batch checksum */
    s->modes=faults;s->all_lost=lost;s->db_failures=(faults&8)?2:0;
    assert(v2_tx_start(&s->tx,p,0));V2TxIO io={s,read_raw,send_packet};
    for(s->now=0;s->now<600000 && v2_tx_active(&s->tx);s->now+=12){
        v2_tx_poll(&s->tx,&io,s->now);delivery(s);
    }
    if(lost || (faults&32))assert(s->tx.phase==V2_TX_ERROR && !s->durable);
    else {
        assert(s->tx.phase==V2_TX_DONE && s->durable && s->stores==1);
        assert(!memcmp(s->saved,raw,points*6));
        if((faults&1) && points>12)assert(s->missing_acks>0);
    }
    printf("transfer: points=%u data=%d faults=%u lost=%d packets=%u simulated_ms=%u OK\n",
           points,mode,faults,lost,s->tx.sent_packets,s->now);
    v2_rx_reset(&s->rx);
}
static void real_data_test(const char *path) {
    FILE *file=fopen(path,"rb");assert(file);
    size_t bytes=fread(raw,1,sizeof(raw),file);
    assert(bytes>0 && bytes%6==0 && fgetc(file)==EOF);fclose(file);
    unsigned points=(unsigned)(bytes/6),compressed_blocks=0,packets=0;
    size_t encoded_bytes=0;
    for(unsigned first=0;first<points;first+=512) {
        unsigned count=points-first;if(count>512)count=512;
        uint8_t codec=0;size_t n=v2_encode(raw+first*6,count,encoded,sizeof(encoded),&codec);
        assert(n && v2_decode(codec,encoded,n,count,decoded,sizeof(decoded)));
        assert(!memcmp(raw+first*6,decoded,count*6));
        encoded_bytes+=n;packets+=v2_fragments((unsigned)n);
        if(codec==V2_RICE)compressed_blocks++;
    }
    printf("REAL DATA: %u XYZ points, %zu -> %zu bytes (%.1f%% retained), %u/%u Rice blocks, %u data fragments; byte-identical roundtrip OK\n",
           points,bytes,encoded_bytes,100.0*encoded_bytes/bytes,compressed_blocks,v2_blocks(points),packets);
}
static void envelope_tests(void) {
    memset(&sim,0,sizeof(sim));
    uint8_t *p=sim.descriptor,reply[V2_ACK_LEN],query[8]={V2_VERSION,V2_KIND_WINDOW};
    V2RxIO io={&sim,is_stored,store_raw,NULL};
    v2_w32(query+2,88);
    assert(v2_rx_receive(&sim.rx,&io,V2_QUERY,query,sizeof(query),reply));
    assert(reply[8]==V2_NEED_BATCH);
    p[0]=V2_VERSION;v2_w32(p+1,88);v2_w16(p+5,26667);v2_w32(p+7,512);v2_w16(p+11,512);
    assert(v2_rx_receive(&sim.rx,&io,V2_BATCH,p,V2_BATCH_LEN,reply));
    assert(reply[8]==V2_OK);
    uint8_t bad[V2_BATCH_LEN];memcpy(bad,p,sizeof(bad));bad[17]^=1;
    assert(v2_rx_receive(&sim.rx,&io,V2_BATCH,bad,sizeof(bad),reply));
    assert(reply[8]==V2_REJECT && !memcmp(sim.rx.batch,p,V2_BATCH_LEN));
    for(size_t n=0;n<V2_BATCH_LEN;n++)assert(!v2_rx_receive(&sim.rx,&io,V2_BATCH,p,n,reply));
    bad[0]=99;assert(!v2_tx_start(&sim.tx,bad,0));
    assert(v2_tx_start(&sim.tx,p,0));
    sim.tx.waiting=1;v2_ack(reply,V2_KIND_BATCH,89,0,V2_OK,0);
    v2_tx_receive(&sim.tx,reply,sizeof(reply),0);assert(sim.tx.phase==V2_TX_BATCH);
    v2_rx_reset(&sim.rx);
    puts("envelopes: unknown metadata, conflict, truncation, version and stale ACK PASS");
}
int main(int argc,char **argv){
    setbuf(stdout,NULL);
    if(argc==2){real_data_test(argv[1]);return 0;}
    codec_tests();
    envelope_tests();
    transfer_test(1,0,0,0);transfer_test(513,1,0,0);
    transfer_test(13334,1,0,0);
    transfer_test(26667,2,0,0);transfer_test(26667,1,0,0);
    transfer_test(40001,1,0,0);
    transfer_test(26667,2,3,0);transfer_test(26667,1,5,0);
    transfer_test(1024,2,8,0);transfer_test(512,0,0,1);
    transfer_test(1024,2,16,0);transfer_test(1024,1,16,0);
    transfer_test(1024,2,32,0);
    puts("V2 protocol tests PASS");return 0;
}
