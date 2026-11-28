#ifndef SPARKPLUG_B_H
#define SPARKPLUG_B_H
#include <stddef.h>
#include <stdint.h>
/* Wire-compatible subset of Eclipse Tahu's sparkplug_b.proto (proto2).
 * https://github.com/eclipse-tahu/tahu/blob/master/sparkplug_b/sparkplug_b.proto
 * Unknown protobuf fields are skipped when decoding commands. */
enum { SPB_INT64=4, SPB_UINT32=7, SPB_UINT64=8, SPB_DOUBLE=10,
       SPB_BOOLEAN=11, SPB_STRING=12, SPB_DATETIME=13, SPB_BYTES=17 };
typedef struct { uint8_t *data; size_t length,capacity; int failed; } SpbBuffer;
typedef struct {
    const char *name,*unit;
    uint64_t alias,timestamp,integer;
    double number;
    const void *data;
    size_t length;
    unsigned datatype;
    int is_null,historical;
} SpbMetric;
void spb_begin(SpbBuffer *out,uint64_t timestamp,int sequence);
int spb_add(SpbBuffer *out,const SpbMetric *metric,int birth);
void spb_free(SpbBuffer *out);
/* Strictly validated Boolean Node Control/Rebirth, by name or NBIRTH alias 1.
 * Returns -1 for malformed/ambiguous payloads, 0 for no request, 1 for true. */
int spb_rebirth_request(const void *payload,size_t length);
#endif
