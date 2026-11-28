#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "zigbee_vibration_store.h"
#include "qspi_nor.h"
static uint8_t flash[8U * 1024U * 1024U];
static uint32_t capacity = sizeof(flash), last_erased;
static unsigned erases;
static int fail_write, fail_read, corrupt_read, fail_erase, power_cut;
static jmp_buf power_cut_return;
uint32_t HAL_GetTick(void) { return 0; }
HAL_StatusTypeDef QspiNor_Init(uint32_t *id, uint32_t *bytes)
{
  if (id) *id = 0xc22817;
  if (bytes) *bytes = capacity;
  return HAL_OK;
}
HAL_StatusTypeDef QspiNor_Read(uint32_t address, uint8_t *data, uint32_t bytes)
{
  assert(address + bytes <= capacity);
  if (fail_read) return HAL_ERROR;
  memcpy(data, flash + address, bytes);
  if (corrupt_read) data[0] ^= 1U;
  return HAL_OK;
}
HAL_StatusTypeDef QspiNor_Write(uint32_t address, const uint8_t *data, uint32_t bytes)
{
  assert(address + bytes <= capacity);
  if (fail_write) return HAL_ERROR;
  for (unsigned i = 0; i < bytes; i++) {
    assert((flash[address+i] & data[i]) == data[i]);
    flash[address+i] &= data[i];
  }
  return HAL_OK;
}
HAL_StatusTypeDef QspiNor_EraseRange(uint32_t address, uint32_t bytes)
{
  uint32_t end = (address + bytes + 4095U) & ~4095U;
  address &= ~4095U;
  assert(end <= capacity);
  last_erased = address; erases++;
  if (power_cut) longjmp(power_cut_return, 1); /* After durable discard, before first erase. */
  if (fail_erase) { memset(flash + address, 255, 4096); return HAL_ERROR; }
  memset(flash + address, 255, end - address);
  return HAL_OK;
}
HAL_StatusTypeDef QspiNor_ProgramPageAsync(uint32_t a, const uint8_t *p, uint16_t n) { return QspiNor_Write(a,p,n); }
HAL_StatusTypeDef QspiNor_ProgramService(void) { return HAL_OK; }
void QspiNor_ProgramAbort(void) { }

/* ACTUAL_STORE */

static void reset_fixture(unsigned slots)
{
  capacity = slots ? slots * ZB_VIB_SLOT_BYTES : sizeof(flash);
  memset(flash, 255, sizeof(flash));
  fail_write = fail_read = corrupt_read = fail_erase = power_cut = 0;
  erases = 0;
  assert(ZigbeeVibrationStore_Init(NULL, NULL) == HAL_OK);
}
static void finish_capture(void)
{
  int16_t samples[160][3] = {{123,456,789}};
  for (unsigned first = 0; first < 13334;) {
    unsigned n = 13334-first; if (n > 160) n = 160;
    assert(ZigbeeVibrationStore_Append(samples,n) == HAL_OK); first += n;
  }
  assert(ZigbeeVibrationStore_Finish(NULL) == HAL_OK);
}
static void capture(uint32_t batch)
{
  assert(ZigbeeVibrationStore_Begin(26667,500,batch) == HAL_OK);
  finish_capture();
}
static int present(uint32_t batch)
{
  uint8_t descriptor[V2_BATCH_LEN]; return ZigbeeVibrationStore_DescribeBatch(batch,descriptor);
}
static void expect_loss(uint32_t old_batch, uint32_t replacement)
{
  ZigbeeVibrationOverwrite_t event;
  assert(ZigbeeVibrationStore_TakeOverwrite(&event));
  assert(event.discarded_batch == old_batch && event.replacement_batch == replacement);
  assert(!ZigbeeVibrationStore_TakeOverwrite(&event));
}
int main(void)
{
  /* An acknowledged slot at the head of the ring must lose to ANY empty slot. */
  reset_fixture(0); capture(42); assert(ZigbeeVibrationStore_Acknowledge(42));
  assert(ZigbeeVibrationStore_Init(NULL,NULL) == HAL_OK); /* Ring now starts at slot 0. */
  capture(43); assert(last_erased == ZB_VIB_SLOT_BYTES);
  for (unsigned i=0; i<14; i++) capture(100+i);
  assert(ZigbeeVibrationStore_FreeSlots() == 1);
  capture(500); assert(last_erased == 0 && present(43)); /* ACK reuse before pending loss. */
  ZigbeeVibrationOverwrite_t event;
  assert(!ZigbeeVibrationStore_TakeOverwrite(&event));

  /* Protect a batch with waveform and scalar metadata through repeated new acquisitions. */
  uint8_t metadata[80] = {1,2,3}, loaded[80], before[6], after[6];
  assert(ZigbeeVibrationStore_SaveMetadata(43,metadata,sizeof(metadata)));
  assert(ZigbeeVibrationStore_ReadBatch(43,0,1,before));
  assert(ZigbeeVibrationStore_ProtectBatch(43));
  assert(ZigbeeVibrationStore_ProtectBatch(43));
  assert(!ZigbeeVibrationStore_ProtectBatch(500)); /* One active upload owner. */
  assert(ZigbeeVibrationStore_Begin(26667,500,43) == HAL_ERROR); /* Duplicate never discards. */
  for (unsigned i=0; i<14; i++) { capture(700+i); expect_loss(100+i,700+i); }
  capture(714); expect_loss(500,714);
  capture(715); expect_loss(700,715); /* Still pins 43 across WAIT/sleep/final-ACK stages. */
  assert(ZigbeeVibrationStore_ReadBatch(43,0,1,after) && !memcmp(before,after,sizeof(before)));
  assert(ZigbeeVibrationStore_LoadMetadata(43,loaded,sizeof(loaded)) == 1 && !memcmp(metadata,loaded,80));
  assert(ZigbeeVibrationStore_ProtectBatch(0));
  capture(716); expect_loss(43,716);
  assert(!present(43) && ZigbeeVibrationStore_LoadMetadata(43,loaded,80) == 0);
  assert(ZigbeeVibrationStore_Init(NULL,NULL) == HAL_OK && !present(43) && present(716));

  /* Sequence, not batch ID / physical slot, defines oldest; rollover is supported. */
  reset_fixture(4); s_next_sequence = UINT32_MAX-1U;
  capture(900); capture(800); capture(700); capture(600);
  assert(ZigbeeVibrationStore_Init(NULL,NULL) == HAL_OK);
  capture(1000); expect_loss(900,1000);
  capture(1001); expect_loss(800,1001);

  /* No safe victim must fail without touching a protected record. */
  reset_fixture(1); capture(1); assert(ZigbeeVibrationStore_ProtectBatch(1));
  unsigned previous = erases;
  assert(ZigbeeVibrationStore_Begin(26667,500,2) == HAL_BUSY && erases == previous);
  assert(present(1) && !ZigbeeVibrationStore_TakeOverwrite(&event));
  assert(ZigbeeVibrationStore_Acknowledge(1)); capture(2); /* Completion releases pin. */
  capture(3); expect_loss(2,3);

  /* Invalidation failure must not initiate destructive erase. */
  reset_fixture(1); capture(1); previous = erases;
  fail_write=1;
  assert(ZigbeeVibrationStore_Begin(26667,500,2) == HAL_ERROR && erases == previous);
  fail_write=0; assert(present(1) && !ZigbeeVibrationStore_TakeOverwrite(&event));
  fail_read=1; assert(ZigbeeVibrationStore_Begin(26667,500,2) == HAL_ERROR && erases == previous);
  fail_read=0; /* Magic may already be invalid: reboot must not resurrect it. */
  assert(ZigbeeVibrationStore_Init(NULL,NULL) == HAL_OK && !present(1));

  reset_fixture(1); capture(1); corrupt_read=1;
  assert(ZigbeeVibrationStore_Begin(26667,500,2) == HAL_ERROR);
  corrupt_read=0;
  assert(ZigbeeVibrationStore_Init(NULL,NULL) == HAL_OK && !present(1));
  reset_fixture(1); capture(1); fail_erase=1;
  assert(ZigbeeVibrationStore_Begin(26667,500,2) == HAL_ERROR); expect_loss(1,2);
  fail_erase=0;
  assert(ZigbeeVibrationStore_Init(NULL,NULL) == HAL_OK && !present(1) && !present(2));
  capture(3); assert(present(3));
  reset_fixture(1); capture(1); power_cut=1;
  if (setjmp(power_cut_return) == 0) {
    (void)ZigbeeVibrationStore_Begin(26667,500,2); assert(!"Expected simulated power cut");
  }
  power_cut=0;
  assert(ZigbeeVibrationStore_Init(NULL,NULL) == HAL_OK && !present(1) && !present(2));
  capture(3); assert(present(3));
  /* Quarantined records are not silently recycled as acknowledged data. */
  reset_fixture(1); capture(1); flash[ZB_VIB_HEADER_OFFSET+12U] ^= 1U;
  assert(ZigbeeVibrationStore_Init(NULL,NULL) == HAL_OK);
  previous=erases;
  assert(ZigbeeVibrationStore_Begin(26667,500,2) == HAL_BUSY && erases == previous);
  puts("Flash overwrite: strict priority, chronological eviction/wrap, active protection, metadata, reboot and write/read/erase/power-loss safety PASS");
  return 0;
}
