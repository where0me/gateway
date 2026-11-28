#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "sampling_cadence.h"
int main(void) {
    SamplingCadence c={0};sampling_cadence_start(&c,1000,300000);
    assert(sampling_cadence_delay(&c,91000)==210000); /* upload took 90 s */
    assert(sampling_cadence_due(&c,301000)==1 && c.next==601000);
    assert(sampling_cadence_due(&c,601000)==1 && c.next==901000); /* busy: skip */
    assert(sampling_cadence_delay(&c,650000)==251000);
    assert(sampling_cadence_due(&c,1501001)==3 && c.next==1801000);
    sampling_cadence_change(&c,600000);
    assert(c.next==2101000); /* anchored to previous planned start */
    sampling_cadence_start(&c,UINT32_MAX-999,300000);
    assert(sampling_cadence_due(&c,299000)==1 && c.next==599000);
    puts("sampling RTC cadence: upload time excluded, skipped deadlines, period change, rollover PASS");
    return 0;
}
