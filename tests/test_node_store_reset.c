#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "zigbee_vibration_store.h"
#include "qspi_nor.h"
static uint8_t flash[4U * 1024U * 1024U];
static int fail_write;
static unsigned erases;
uint32_t HAL_GetTick(void) { return 0U; }
HAL_StatusTypeDef QspiNor_Init(uint32_t *id, uint32_t *capacity) {
  if (id) *id = 0xC22817;
  if (capacity) *capacity = sizeof(flash);
  return HAL_OK;
}
HAL_StatusTypeDef QspiNor_Read(uint32_t address, uint8_t *data, uint32_t n) {
  assert(address + n <= sizeof(flash));
  memcpy(data, flash + address, n); return HAL_OK;
}
HAL_StatusTypeDef QspiNor_Write(uint32_t address, const uint8_t *data, uint32_t n) {
  assert(address + n <= sizeof(flash));
  if (fail_write) return HAL_ERROR;
  for (unsigned i = 0; i < n; ++i) {
    assert((flash[address+i] & data[i]) == data[i]);
    flash[address+i] &= data[i];
  }
  return HAL_OK;
}
/* This regression exercises synchronous records; async behavior has its own test. */
HAL_StatusTypeDef QspiNor_ProgramPageAsync(uint32_t address, const uint8_t *data, uint16_t n) {
  return QspiNor_Write(address, data, n);
}
HAL_StatusTypeDef QspiNor_ProgramService(void) { return HAL_OK; }
void QspiNor_ProgramAbort(void) {}
HAL_StatusTypeDef QspiNor_EraseRange(uint32_t address, uint32_t n) {
  unsigned end = (address + n + 4095U) & ~4095U;
  address &= ~4095U;
  assert(end <= sizeof(flash));
  memset(flash + address, 255, end - address); ++erases; return HAL_OK;
}
static void capture(uint32_t batch) {
  int16_t samples[160][3] = {{123,456,789}};
  assert(ZigbeeVibrationStore_Begin(26667, 500, batch) == HAL_OK);
  for (unsigned first = 0; first < 13334;) {
    unsigned n = 13334 - first;
    if (n > 160) n = 160;
    assert(ZigbeeVibrationStore_Append(samples, n) == HAL_OK); first += n;
  }
  assert(ZigbeeVibrationStore_Finish(NULL) == HAL_OK);
}
int main(void) {
  memset(flash, 255, sizeof(flash));
  assert(ZigbeeVibrationStore_Init(NULL, NULL) == HAL_OK);
  unsigned slots = ZigbeeVibrationStore_FreeSlots();
  capture(1); capture(2);
  assert(ZigbeeVibrationStore_Acknowledge(1));
  uint8_t descriptor[V2_BATCH_LEN];
  assert(ZigbeeVibrationStore_Pending(descriptor));
  uint8_t old_data[6]; memcpy(old_data, flash + 4096, 6);
  unsigned before = erases;
  fail_write = 1;
  assert(ZigbeeVibrationStore_ResetRecords() == HAL_ERROR);
  assert(!ZigbeeVibrationStore_IsReady());
  fail_write = 0;
  assert(ZigbeeVibrationStore_ResetRecords() == HAL_OK);
  assert(erases == before); /* No full/range data erase during cleanup. */
  assert(memcmp(old_data, flash + 4096, 6) == 0);
  assert(ZigbeeVibrationStore_Init(NULL, NULL) == HAL_OK); /* Power cycle. */
  assert(!ZigbeeVibrationStore_Pending(descriptor));
  assert(!ZigbeeVibrationStore_DescribeBatch(1, descriptor));
  assert(!ZigbeeVibrationStore_DescribeBatch(2, descriptor));
  assert(ZigbeeVibrationStore_FreeSlots() == slots);
  assert(ZigbeeVibrationStore_ResetRecords() == HAL_OK); /* Idempotent. */
  capture(3);
  assert(ZigbeeVibrationStore_DescribeBatch(3, descriptor));
  assert(erases == before + 1);
  puts("node logical reset: failure/retry, reboot, retained bytes and new capture PASS");
  return 0;
}
