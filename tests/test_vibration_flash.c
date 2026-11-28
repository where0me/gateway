#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include "vibration_monitor.h"
#include "qspi_nor.h"
#include "vibration.h"

#define CAPACITY (1024U*1024U)
#define SLOT_BYTES (60U*4096U)
static uint8_t flash[CAPACITY], before[CAPACITY], block[V2_RAW_BYTES];
static uint32_t fifo_at;
static int fail_commit, fail_read;
HAL_StatusTypeDef QspiNor_Init(uint32_t *jedec,uint32_t *capacity) {
    *jedec=0xEF4014;*capacity=CAPACITY;return HAL_OK;
}
uint32_t QspiNor_GetCapacity(void){return CAPACITY;}
HAL_StatusTypeDef QspiNor_Read(uint32_t address,uint8_t *data,uint32_t length) {
    if(fail_read)return HAL_ERROR;
    assert(address<=CAPACITY && length<=CAPACITY-address);
    memcpy(data,flash+address,length);return HAL_OK;
}
HAL_StatusTypeDef QspiNor_Write(uint32_t address,const uint8_t *data,uint32_t length) {
    assert(address<=CAPACITY && length<=CAPACITY-address);
    if(fail_commit && address%SLOT_BYTES==128 && length==4)return HAL_ERROR;
    for(uint32_t i=0;i<length;i++){assert((flash[address+i]&data[i])==data[i]);flash[address+i]&=data[i];}
    return HAL_OK;
}
HAL_StatusTypeDef QspiNor_EraseRange(uint32_t address,uint32_t length) {
    assert(address%4096==0 && address+length<=CAPACITY);
    length=(length+4095)/4096*4096;memset(flash+address,0xFF,length);return HAL_OK;
}
HAL_StatusTypeDef Vibration_InitFifo(void){fifo_at=0;return HAL_OK;}
HAL_StatusTypeDef Vibration_EnterIdle(void){return HAL_OK;}
HAL_StatusTypeDef Vibration_ReadFifo(Vibration_Sample_t *samples,uint16_t capacity,uint16_t *count) {
    *count=capacity;
    for(unsigned i=0;i<capacity;i++,fifo_at++){
        samples[i].x_raw=(int16_t)fifo_at;samples[i].y_raw=(int16_t)(-((int32_t)fifo_at));
        samples[i].z_raw=4;
    }
    return HAL_OK;
}
static void capture(unsigned batch){
    assert(VibrationMonitor_Start(26667,1000,batch)==HAL_OK);
    while(VibrationMonitor_GetState()==VIBRATION_MONITOR_CAPTURING)VibrationMonitor_Process();
    assert(VibrationMonitor_GetState()==VIBRATION_MONITOR_COMPLETE);
    VibrationMonitor_Stop();
}
int main(void) {
    memset(flash,0xFF,sizeof(flash));
    assert(VibrationMonitor_Init()==HAL_OK);
    unsigned slots=VibrationMonitor_FreeSlots();assert(slots==4);
    capture(1);memcpy(before,flash,sizeof(flash));
    for(unsigned i=2;i<=slots;i++)capture(i);
    assert(!memcmp(before,flash,SLOT_BYTES));
    assert(VibrationMonitor_FreeSlots()==0);
    memcpy(before,flash,sizeof(flash));
    assert(VibrationMonitor_Start(26667,1000,20)==HAL_BUSY);
    assert(!memcmp(before,flash,sizeof(flash)));
    assert(VibrationMonitor_Init()==HAL_OK); /* reset: pending survives */
    uint8_t descriptor[V2_BATCH_LEN];
    assert(VibrationMonitor_Pending(descriptor) && v2_u32(descriptor+1)==1);
    uint32_t crc=UINT32_MAX;
    for(unsigned first=0;first<26667;first+=512) {
        unsigned points=26667-first;if(points>512)points=512;
        assert(VibrationMonitor_ReadBatch(1,first,points,block));
        crc=v2_crc_update(crc,block,points*6);
    }
    assert((crc^UINT32_MAX)==v2_u32(descriptor+13));
    assert(VibrationMonitor_Acknowledge(1));
    assert(VibrationMonitor_Init()==HAL_OK);
    assert(VibrationMonitor_FreeSlots()==1);
    assert(VibrationMonitor_Pending(descriptor) && v2_u32(descriptor+1)==2);
    capture(20);
    assert(VibrationMonitor_FreeSlots()==0);
    assert(VibrationMonitor_DiscardPending());
    assert(VibrationMonitor_Init()==HAL_OK && VibrationMonitor_FreeSlots()==slots);
    fail_commit=1;
    assert(VibrationMonitor_Start(26667,1000,21)==HAL_OK);
    while(VibrationMonitor_GetState()==VIBRATION_MONITOR_CAPTURING)VibrationMonitor_Process();
    assert(VibrationMonitor_GetState()==VIBRATION_MONITOR_ERROR);
    fail_commit=0;
    assert(VibrationMonitor_Init()==HAL_OK);
    assert(!VibrationMonitor_Pending(descriptor)); /* torn metadata is not a valid batch */
    fail_read=1;assert(VibrationMonitor_Init()==HAL_ERROR);fail_read=0;
    /* Migrate old single-slot header without erasing its raw bytes. */
    memset(flash,0xFF,sizeof(flash));
    for(unsigned i=0;i<300;i++)flash[4096+i]=(uint8_t)i;
    uint32_t legacy[6]={0x56494231U,77,26667,50,300,0};
    legacy[5]=v2_crc(flash+4096,300);
    memcpy(flash,legacy,sizeof(legacy));
    memcpy(before,flash,sizeof(flash));
    assert(VibrationMonitor_Init()==HAL_OK);
    assert(VibrationMonitor_Pending(descriptor) && v2_u32(descriptor+1)==77);
    assert(!memcmp(before+4096,flash+4096,300));
    assert(VibrationMonitor_ReadBatch(77,0,50,block));
    assert(!memcmp(block,before+4096,300));
    /* Committed header corruption is quarantined, not reclaimed. */
    flash[128+8]^=1;
    assert(VibrationMonitor_Init()==HAL_OK);
    assert(!VibrationMonitor_Pending(descriptor));
    assert(VibrationMonitor_FreeSlots()==slots-1);
    puts("Flash: 4 slots (1 MiB / 60 sectors), no overwrite, reset recovery, CRC, ACK release, full, removal, torn commit PASS");
    puts("Flash: legacy migration and corrupted-header quarantine PASS");
    return 0;
}
