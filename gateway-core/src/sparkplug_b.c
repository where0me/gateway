#include "sparkplug_b.h"
#include <stdlib.h>
#include <string.h>
#define SPB_LIMIT (2U*1024U*1024U)
static void append(SpbBuffer *b,const void *data,size_t n) {
    if(b->failed || n>SPB_LIMIT-b->length){b->failed=1;return;}
    if(b->length+n>b->capacity) {
        size_t cap=b->capacity?b->capacity:256;
        while(cap<b->length+n)cap*=2;
        void *p=realloc(b->data,cap);
        if(!p){b->failed=1;return;}b->data=p;b->capacity=cap;
    }
    if(n) {
        memcpy(b->data+b->length,data,n);
    }
    b->length+=n;
}
static void varint(SpbBuffer *b,uint64_t v) {
    uint8_t bytes[10];size_t n=0;
    do{bytes[n++]=(uint8_t)((v&127U)|(v>127U?128U:0U));v>>=7;}while(v);
    append(b,bytes,n);
}
static void integer(SpbBuffer *b,unsigned field,uint64_t v){varint(b,(uint64_t)field<<3);varint(b,v);}
static void bytes(SpbBuffer *b,unsigned field,const void *p,size_t n) {
    varint(b,((uint64_t)field<<3)|2U);varint(b,n);append(b,p,n);
}
static void text(SpbBuffer *b,unsigned field,const char *s){bytes(b,field,s,strlen(s));}
void spb_free(SpbBuffer *b){free(b->data);memset(b,0,sizeof(*b));}
void spb_begin(SpbBuffer *b,uint64_t timestamp,int seq) {
    memset(b,0,sizeof(*b));integer(b,1,timestamp);
    if(seq>=0)integer(b,3,(unsigned)seq&255U);
}
int spb_add(SpbBuffer *out,const SpbMetric *m,int birth) {
    SpbBuffer b={0};
    if(birth || !m->alias)text(&b,1,m->name);
    if(m->alias)integer(&b,2,m->alias);
    integer(&b,3,m->timestamp);integer(&b,4,m->datatype);
    if(m->historical)integer(&b,5,1);
    if(m->is_null)integer(&b,7,1);
    if(m->unit && birth) {
        SpbBuffer properties={0},value={0};
        text(&properties,1,"engUnit");integer(&value,1,SPB_STRING);text(&value,8,m->unit);
        bytes(&properties,2,value.data,value.length);bytes(&b,9,properties.data,properties.length);
        if(value.failed || properties.failed)b.failed=1;
        spb_free(&value);spb_free(&properties);
    }
    if(!m->is_null)switch(m->datatype) {
        case SPB_UINT32:integer(&b,10,(uint32_t)m->integer);break;
        case SPB_INT64:case SPB_UINT64:case SPB_DATETIME:integer(&b,11,m->integer);break;
        case SPB_BOOLEAN:integer(&b,14,!!m->integer);break;
        case SPB_STRING:bytes(&b,15,m->data,m->length);break;
        case SPB_BYTES:bytes(&b,16,m->data,m->length);break;
        case SPB_DOUBLE: {
            uint64_t bits;uint8_t little[8];memcpy(&bits,&m->number,8);
            for(unsigned i=0;i<8;i++)little[i]=(uint8_t)(bits>>(8*i));
            varint(&b,((uint64_t)13<<3)|1U);append(&b,little,8);break;
        }
        default:b.failed=1;break;
    }
    if(b.failed)out->failed=1;else bytes(out,2,b.data,b.length);
    spb_free(&b);return !out->failed;
}
typedef struct {const uint8_t *p,*end;} Reader;
static int readvar(Reader *r,uint64_t *v) {
    *v=0;
    for(unsigned shift=0;shift<70;shift+=7) {
        if(r->p==r->end)return 0;
        uint8_t b=*r->p++;if(shift==63 && b>1)return 0;
        *v|=(uint64_t)(b&127U)<<shift;
        if(!(b&128U))return 1;
    }
    return 0;
}
static int field(Reader *r,unsigned *key,uint64_t *v,Reader *slice) {
    uint64_t k,n;if(!readvar(r,&k) || k<8 || k>UINT32_MAX)return 0;*key=(unsigned)k;
    *slice=(Reader){NULL,NULL};
    if((k&7U)==0)return readvar(r,v);
    if((k&7U)==2){if(!readvar(r,&n) || n>(uint64_t)(r->end-r->p))return 0;}
    else if((k&7U)==1)n=8;else if((k&7U)==5)n=4;else return 0;
    if(n>(uint64_t)(r->end-r->p))return 0;
    *slice=(Reader){r->p,r->p+(size_t)n};r->p+=(size_t)n;return 1;
}
static int command_metric(Reader r,int *request) {
    uint64_t alias=0,type=0,value=0;unsigned present=0;int name_match=0,has_name=0,is_null=0;
    while(r.p<r.end) {
        unsigned k;uint64_t v=0;Reader s;if(!field(&r,&k,&v,&s))return 0;
        unsigned mask=k==10?1U:k==16?2U:k==32?4U:k==112?8U:k==56?16U:0;
        if(mask && (present&mask)) {
            return 0;
        }
        present|=mask;
        if(k==10){has_name=1;name_match=s.end-s.p==20 && !memcmp(s.p,"Node Control/Rebirth",20);}
        else if(k==16)alias=v;
        else if(k==32)type=v;
        else if(k==112)value=v;
        else if(k==56)is_null=v!=0;
        /* Reject a competing oneof value, not silently reinterpret it. */
        else if((k>>3)>=10 && (k>>3)<=19)return 0;
    }
    if(name_match || (!has_name && alias==1)) {
        if((has_name && alias && alias!=1) || ((present&4U) && type!=SPB_BOOLEAN) || !(present&8) || value>1 || is_null)return 0;
        if(value){if(*request)return 0;*request=1;}
    }
    return 1;
}
int spb_rebirth_request(const void *p,size_t n) {
    if(!p || !n || n>8192)return -1;
    Reader r={p,(const uint8_t *)p+n};int request=0;
    while(r.p<r.end){unsigned k;uint64_t v=0;Reader s;if(!field(&r,&k,&v,&s))return -1;
        if(k==18 && !command_metric(s,&request))return -1;
        if((k>>3)==2 && k!=18)return -1;
    }
    return request;
}
