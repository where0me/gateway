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
HAL_StatusTypeDef QspiNor_EraseRange(uint32_t address, uint32_t n) {
  unsigned end = (address + n + 4095U) & ~4095U;
  address &= ~4095U;
  assert(end <= sizeof(flash));
  memset(flash + address, 255, end - address); ++erases; return HAL_OK;
}

static const uint8_t *dma_source;
static uint8_t dma_snapshot[256];
static uint32_t dma_address;
static uint16_t dma_size;
static unsigned dma_wait;
HAL_StatusTypeDef QspiNor_ProgramPageAsync(uint32_t address, const uint8_t *data, uint16_t length) {
  assert(!dma_source && length && length <= 256 && (address % 256) + length <= 256);
  dma_source = data; dma_address = address; dma_size = length; dma_wait = 3;
  memcpy(dma_snapshot, data, length);
  return HAL_OK;
}
HAL_StatusTypeDef QspiNor_ProgramService(void) {
  if (!dma_source) return HAL_OK;
  assert(!memcmp(dma_source, dma_snapshot, dma_size)); /* DMA buffer immutable */
  if (fail_write) { dma_source = NULL; return HAL_ERROR; }
  if (dma_wait--) return HAL_BUSY;
  HAL_StatusTypeDef result = QspiNor_Write(dma_address, dma_source, dma_size);
  dma_source = NULL;
  return result;
}
void QspiNor_ProgramAbort(void) { dma_source = NULL; }
static uint8_t expected[40001 * 6];
static void capture_async(uint32_t batch, uint16_t window, unsigned total) {
  int16_t samples[42][3];
  assert(ZigbeeVibrationStore_Begin(26667, window, batch) == HAL_OK);
  for (unsigned first = 0, guard = 0; first < total; ++guard) {
    assert(guard < 200000);
    assert(ZigbeeVibrationStore_Service(0) != HAL_ERROR);
    unsigned n = ZigbeeVibrationStore_AsyncCapacity();
    if (n > total - first) n = total - first;
    if (!n) continue;
    for (unsigned i=0; i<n; ++i)
      for (unsigned axis=0; axis<3; ++axis) samples[i][axis] = (int16_t)((first+i)*3+axis);
    memcpy(expected + first*6, samples, n*6);
    assert(ZigbeeVibrationStore_AppendAsync(samples, n) == HAL_OK);
    first += n;
  }
  assert(ZigbeeVibrationStore_SampleCount() == total);
  unsigned guard = 0;
  HAL_StatusTypeDef status;
  do { status = ZigbeeVibrationStore_Service(1); assert(++guard < 100); } while (status == HAL_BUSY);
  assert(status == HAL_OK);
  assert(ZigbeeVibrationStore_Finish(NULL) == HAL_OK);
  uint8_t readback[160*6];
  for (unsigned first=0; first<total;) {
    unsigned n = total-first; if(n>160) n=160;
    assert(ZigbeeVibrationStore_ReadBatch(batch, first, n, readback));
    assert(!memcmp(readback, expected+first*6, n*6)); first+=n;
  }
  ZigbeeVibrationWriteStats_t stats;
  ZigbeeVibrationStore_GetWriteStats(&stats);
  assert(stats.bytes == total*6 && stats.pages == (total*6+255)/256);
  assert(ZigbeeVibrationStore_Acknowledge(batch));
}
int main(void) {
  memset(flash, 255, sizeof(flash));
  assert(ZigbeeVibrationStore_Init(NULL, NULL) == HAL_OK);
  capture_async(1,500,13334);
  capture_async(2,1000,26667);
  capture_async(3,1500,40001);
  int16_t samples[42][3] = {{1,2,3}};
  assert(ZigbeeVibrationStore_Begin(26667,1500,4) == HAL_OK);
  assert(ZigbeeVibrationStore_AppendAsync(samples,42) == HAL_OK);
  assert(ZigbeeVibrationStore_AppendAsync(samples,42) == HAL_OK);
  assert(ZigbeeVibrationStore_Finish(NULL) == HAL_BUSY);
  fail_write=1;
  assert(ZigbeeVibrationStore_Service(0) == HAL_ERROR);
  ZigbeeVibrationStore_Abort();
  fail_write=0;
  capture_async(5,1500,40001);
  puts("async store: 0.5/1/1.5s byte equality, page boundaries, immutable DMA buffers, final drain, failure/abort/retry PASS");
  return 0;
}
