#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "vibration_features.h"
#include "vibration_codec.h"

static uint8_t raw[40001U*6U], encoded[V2_RAW_BYTES];
int main(void) {
  const unsigned counts[]={13334U,26667U,40001U};
  for(unsigned trial=0;trial<3;trial++) {
    unsigned count=counts[trial];
    for(unsigned i=0;i<count;i++) for(unsigned axis=0;axis<3;axis++)
      v2_w16(raw+i*6U+axis*2U,(uint16_t)(int16_t)((int)(i*97U+axis*719U)%32000-16000));
    VibFeatures whole,upload,skip;
    int32_t expected[3][5],a[3][5],b[3][5];
    vib_features_begin(&whole,42,count);
    assert(vib_features_add(&whole,42,0,count,raw));
    assert(vib_features_finish(&whole,26667,expected));
    vib_features_begin(&upload,42,count);
    vib_features_begin(&skip,42,count);
    assert(!vib_features_finish(&upload,26667,a));
    assert(!vib_features_add(&upload,43,0,1,raw));
    assert(!vib_features_add(&upload,42,1,1,raw));
    for(unsigned first=0;first<count;first+=V2_POINTS) {
      unsigned points=count-first;
      if(points>V2_POINTS)points=V2_POINTS;
      assert(vib_features_add(&upload,42,first,points,raw+first*6U));
      uint8_t codec;
      assert(v2_encode(raw+first*6U,points,encoded,sizeof(encoded),&codec));
      VibFeatures before=upload;
      /* Block replay after a missing ACK / NEED_BATCH does not double count. */
      assert(vib_features_add(&upload,42,first,points,raw+first*6U));
      assert(!memcmp(&before,&upload,sizeof(before)));
      assert(vib_features_add(&skip,42,first,points,raw+first*6U));
    }
    assert(vib_features_finish(&upload,26667,a));
    assert((upload.raw_crc ^ UINT32_MAX)==v2_crc(raw,(size_t)count*6U));
    assert(vib_features_finish(&skip,26667,b));
    assert(!memcmp(expected,a,sizeof(a)) && !memcmp(a,b,sizeof(a)));
    assert(!vib_features_add(&skip,42,count,1,raw));
  }
  for(unsigned i=0;i<1024;i++) for(unsigned axis=0;axis<3;axis++)
    v2_w16(raw+i*6U+axis*2U,(uint16_t)(int16_t)((i&1)?16000:-16000));
  VibFeatures known;
  int32_t values[3][5];
  vib_features_begin(&known,7,1024);
  assert(vib_features_add(&known,7,0,1024,raw));
  assert(vib_features_finish(&known,26667,values));
  assert(values[0][0]==7808 && values[0][1]==7808 &&
         values[0][2]==15616 && values[0][3]==1000);
  puts("deferred features: whole/512-point windows equal, replay-safe, skip-raw equal PASS");
  return 0;
}
