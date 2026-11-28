#ifndef SCALAR_DB_H
#define SCALAR_DB_H
#include "scalar_delivery.h"
#include <math.h>
/* Reconstruct the integer wire values from durable per-batch SQLite values.
 * No latest-shadow fallback: another batch must never satisfy this check. */
static int scalar_db_integer(float value,double scale,int16_t *out)
{
    double raw=(double)value*scale;
    if(!isfinite(raw) || raw < -32768.49 || raw > 32767.49)return 0;
    *out=(int16_t)(raw<0 ? raw-0.5 : raw+0.5);return 1;
}
static uint8_t scalar_db_missing(const char *mac,const uint8_t manifest[SCALAR_MANIFEST_LEN])
{
    float values[6]={0};unsigned present=0;uint8_t matched=0;
    uint32_t batch=scalar_u32(manifest+1);
    /* Only captured values can be repaired. Declared acquisition failures are
     * recorded separately and must never be requested over the radio. */
    uint8_t expected=manifest[6]&manifest[7];
    if(!get_collection_values(mac,batch,&values[0],&values[1],&values[2],&values[3],
                              &values[4],&values[5],&present))return expected;
    for(unsigned item=0;item<4;item++) {
        unsigned required=item==0?1U:item==1?14U:item==2?16U:32U;
        if(!(expected&(1U<<item)) || (present&required)!=required)continue;
        uint8_t payload[10]={0};scalar_w32(payload,batch);int valid=1;
        unsigned words=item==1?3U:1U;
        for(unsigned axis=0;axis<words;axis++) {
            unsigned field=item==0?0U:item==1?axis+1U:item+2U;
            int16_t raw=0;
            if(!scalar_db_integer(values[field],item==1?1.0/0.15:100.0,&raw))valid=0;
            payload[4+2*axis]=(uint8_t)raw;payload[5+2*axis]=(uint8_t)((uint16_t)raw>>8);
        }
        if(valid && scalar_value_crc(item,payload)==scalar_u32(manifest+8+4*item))matched|=(uint8_t)(1U<<item);
    }
    return expected & (uint8_t)~matched;
}
#endif
