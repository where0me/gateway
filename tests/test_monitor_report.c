#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gateway_protocol.h"
int main(void)
{
    uint8_t payload[7] = {1,0,0x78,0x56,0x34,0x12,4};
    uint8_t out[12], wire[259];
    for (unsigned size=6;size<=7;size++) for (unsigned state=0;state<256;state++) {
        memset(out,0xcc,sizeof(out)); payload[1]=(uint8_t)state;
        uint8_t length=Gateway_MonitorReport(out,0x4321,payload,size,149,-46);
        assert(length==(state==0 ? 11 : 8));
        assert(Gateway_LE16(out)==0x4321 && memcmp(out+2,payload,6)==0);
        if (state==0) assert(out[8]==(size==7 ? 4 : 0) && out[9]==149 && (int8_t)out[10]==-46);
        else assert(out[8]==0xcc && out[9]==0xcc && out[10]==0xcc);
        assert(out[11]==0xcc);
        GatewayParser parser={0};
        uint16_t encoded=Gateway_Encode(0x8d,out,length,wire);
        for(unsigned i=0;i<encoded;i++) assert(Gateway_ParseByte(&parser,wire[i])==(i==encoded-1));
        assert(parser.frame.cmd==0x8d && parser.frame.len==length && memcmp(parser.frame.data,out,length)==0);
    }
    assert(Gateway_MonitorReport(out,0x4321,payload,5,149,-46)==0);
    assert(Gateway_MonitorReport(out,0x4321,payload,8,149,-46)==0);
    puts("Actual TI monitor encoder: idle telemetry only; lifecycle payload/BCC preserved for all states PASS");
    return 0;
}
