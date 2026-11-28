#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
static uint8_t permitDuration,permitContinuous;
static uint32_t permitDeadline,permitRenewAt,now;
static unsigned submissions,lastDuration,stateActive;
static uint32_t millis(void){return now;}
static bool due(uint32_t a,uint32_t b){return (int32_t)(a-b)>=0;}
static uint8_t permit(uint8_t duration,uint8_t source)
{(void)source;submissions++;lastDuration=duration;return 0;}
static void emit(uint8_t cmd,const uint8_t *p,uint8_t size)
{assert(cmd==0xa1 && size==2);stateActive=p[0];}
static uint8_t permitWanted(uint8_t);
static void permitTick(uint32_t);
static void permitState(void);
int main(void)
{
    now=100;permitWanted(1);assert(permitContinuous && lastDuration==254 && submissions==1);
    permitState();assert(!stateActive);now=5100;permitTick(now);assert(submissions==2);
    permitDuration=254;permitDeadline=now+254000;permitRenewAt=now+224000;
    permitState();assert(stateActive);now+=223999;permitTick(now);assert(submissions==2);
    now++;permitTick(now);assert(submissions==3 && lastDuration==254);
    permitWanted(0);assert(!permitContinuous && lastDuration==0);
    now+=1000000;permitTick(now);assert(submissions==4);
    now=UINT32_MAX-1000U;permitWanted(1);now+=4999U;permitTick(now);assert(submissions==5);
    now++;permitTick(now);assert(submissions==6);
    puts("Continuous permit PASS: stack-confirmed visibility, retry, renewal, manual close cancellation and clock wrap");
}
