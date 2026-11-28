#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "zigbee_vibration_store.h"
#include "qspi_nor.h"
static uint8_t flash[4U*1024U*1024U];
static int fail_write;
uint32_t HAL_GetTick(void) { return 0U; }
HAL_StatusTypeDef QspiNor_Init(uint32_t *id,uint32_t *capacity) {
  if(id)*id=0xC22817;if(capacity)*capacity=sizeof(flash);return HAL_OK;
}
HAL_StatusTypeDef QspiNor_Read(uint32_t address,uint8_t *data,uint32_t n) {
  assert(address+n<=sizeof(flash));memcpy(data,flash+address,n);return HAL_OK;
}
HAL_StatusTypeDef QspiNor_Write(uint32_t address,const uint8_t *data,uint32_t n) {
  assert(address+n<=sizeof(flash));if(fail_write)return HAL_ERROR;
  for(unsigned i=0;i<n;i++) {
    assert((flash[address+i]&data[i])==data[i]);flash[address+i]&=data[i];
  }
  return HAL_OK;
}
HAL_StatusTypeDef QspiNor_EraseRange(uint32_t address,uint32_t n) {
  unsigned end=(address+n+4095U)&~4095U;address&=~4095U;
  assert(end<=sizeof(flash));memset(flash+address,255,end-address);return HAL_OK;
}
/* Host fixture implements storage semantics, not real DMA/IRQ timing. */
HAL_StatusTypeDef QspiNor_ProgramPageAsync(uint32_t address,const uint8_t *data,uint16_t n) {
  return QspiNor_Write(address,data,n);
}
HAL_StatusTypeDef QspiNor_ProgramService(void) { return HAL_OK; }
void QspiNor_ProgramAbort(void) { }
int main(void) {
  memset(flash,255,sizeof(flash));
  assert(ZigbeeVibrationStore_Init(NULL,NULL)==HAL_OK);
  unsigned initial=ZigbeeVibrationStore_FreeSlots();
  assert(ZigbeeVibrationStore_Begin(26667,500,42)==HAL_OK);
  int16_t samples[160][3]={{0}};
  for(unsigned first=0;first<13334;) {
    unsigned n=13334-first;if(n>160)n=160;
    assert(ZigbeeVibrationStore_Append(samples,n)==HAL_OK);first+=n;
  }
  assert(ZigbeeVibrationStore_Finish(NULL)==HAL_OK);
  uint8_t descriptor[V2_BATCH_LEN],raw[512*6];
  assert(ZigbeeVibrationStore_DescribeBatch(42,descriptor));
  uint8_t metadata[80]={1,2,3},loaded[80];
  metadata[78]=15;metadata[79]=1; /* New capture policy occupies old padding. */
  assert(ZigbeeVibrationStore_LoadMetadata(42,loaded,sizeof(loaded))==0);
  fail_write=1;assert(!ZigbeeVibrationStore_SaveMetadata(42,metadata,sizeof(metadata)));fail_write=0;
  assert(ZigbeeVibrationStore_SaveMetadata(42,metadata,sizeof(metadata)));
  assert(ZigbeeVibrationStore_SaveMetadata(42,metadata,sizeof(metadata)));
  assert(ZigbeeVibrationStore_Init(NULL,NULL)==HAL_OK);
  assert(ZigbeeVibrationStore_LoadMetadata(42,loaded,sizeof(loaded))==1 && !memcmp(metadata,loaded,sizeof(loaded)));
  flash[1280U+12U]^=1U;
  assert(ZigbeeVibrationStore_LoadMetadata(42,loaded,sizeof(loaded))==-1);
  flash[1280U+12U]^=1U;
  metadata[0]=9;assert(!ZigbeeVibrationStore_SaveMetadata(42,metadata,sizeof(metadata)));
  assert(ZigbeeVibrationStore_LoadMetadata(42,loaded,10)==-1);
  assert(ZigbeeVibrationStore_LoadMetadata(999,loaded,sizeof(loaded))==0);
  int32_t features[3][5];
  for(unsigned a=0;a<3;a++)for(unsigned f=0;f<5;f++)
    features[a][f]=(int32_t)v2_u32(descriptor+17+(a*5+f)*4);
  assert(v2_features_pending(features));
  assert(ZigbeeVibrationStore_ReadBatch(42,0,512,raw));
  assert(ZigbeeVibrationStore_RawPolicy(42)==1);
  assert(ZigbeeVibrationStore_SetRawPolicy(42,0));
  assert(ZigbeeVibrationStore_RawPolicy(42)==0);
  memset(features,0,sizeof(features));
  fail_write=1;
  assert(!ZigbeeVibrationStore_SaveFeatures(42,features));
  assert(ZigbeeVibrationStore_Pending(descriptor));
  fail_write=0;
  assert(ZigbeeVibrationStore_SaveFeatures(42,features));
  assert(ZigbeeVibrationStore_SaveFeatures(42,features));
  assert(ZigbeeVibrationStore_Init(NULL,NULL)==HAL_OK);
  assert(ZigbeeVibrationStore_RawPolicy(42)==0);
  assert(ZigbeeVibrationStore_SaveFeatures(42,features));
  features[0][0]=1;
  assert(!ZigbeeVibrationStore_SaveFeatures(42,features));
  assert(ZigbeeVibrationStore_FreeSlots()==initial-1);
  /* A new sampling deadline while batch 42 is still waiting must allocate a
   * different slot, preserving both its waveform and associated scalar data. */
  assert(ZigbeeVibrationStore_Begin(26667,500,43)==HAL_OK);
  assert(ZigbeeVibrationStore_ReadBatch(42,0,512,raw));
  for(unsigned first=0;first<13334;) {
    unsigned n=13334-first;if(n>160)n=160;
    assert(ZigbeeVibrationStore_Append(samples,n)==HAL_OK);first+=n;
  }
  assert(ZigbeeVibrationStore_Finish(NULL)==HAL_OK);
  uint8_t second_metadata[80]={7,8,9};
  assert(ZigbeeVibrationStore_SaveMetadata(43,second_metadata,sizeof(second_metadata)));
  assert(ZigbeeVibrationStore_Init(NULL,NULL)==HAL_OK);
  assert(ZigbeeVibrationStore_Pending(descriptor) && v2_u32(descriptor+1)==42);
  assert(ZigbeeVibrationStore_LoadMetadata(42,loaded,sizeof(loaded))==1 && loaded[0]==1);
  assert(ZigbeeVibrationStore_Acknowledge(42));
  assert(ZigbeeVibrationStore_Pending(descriptor) && v2_u32(descriptor+1)==43);
  assert(ZigbeeVibrationStore_LoadMetadata(43,loaded,sizeof(loaded))==1 && loaded[0]==7);
  assert(ZigbeeVibrationStore_Acknowledge(43));
  assert(!ZigbeeVibrationStore_Pending(descriptor));
  assert(ZigbeeVibrationStore_FreeSlots()==initial);
  puts("fusion Flash: deferred metadata, policy restart, feature commit/retry, retain-until-ACK PASS");
  return 0;
}
