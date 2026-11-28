/* Hardware fault-injection harness for the actual STM32WB EEPROM driver. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hw_flash.h"
#include "ee.h"

#define FLASH_BASE 0x08000000U
#define FLASH_PAGE_SIZE 4096U
#define CFG_NVM_BASE_ADDRESS 0x70000U
#define CFG_EE_BANK0_SIZE 65536U
#define CFG_EE_BANK0_MAX_NB 1000U
#define CFG_EE_AUTO_CLEAN 1U
#define CFG_HW_FLASH_SEMID 2U
#define CFG_HW_BLOCK_FLASH_REQ_BY_CPU1_SEMID 6U
#define CFG_HW_BLOCK_FLASH_REQ_BY_CPU2_SEMID 7U
#define CFG_LPM_NVM_FLASH 6U
#define UTIL_LPM_DISABLE 0U
#define UTIL_LPM_ENABLE 1U
#define FLASH_ACR_DCEN 1U
#define FLASH_FLAG_PESD 1U
#define FLASH_FLAG_BSY 2U
#define FLASH_FLAG_CFGBSY 4U
#define FLASH_FLAG_OPTVERR 8U
#define FLASH_TYPEERASE_PAGES 0U
#define FLASH_TYPEPROGRAM_DOUBLEWORD 0U
#define ERASE_ACTIVITY_ON 1U
#define ERASE_ACTIVITY_OFF 0U
#define SHCI_Success 0U
#define UNUSED(x) ((void)(x))
#define READ_BIT(reg, mask) ((reg) & (mask))
#define __HAL_FLASH_CLEAR_FLAG(mask) (peripheral.SR &= ~(mask))
#define __HAL_FLASH_DATA_CACHE_DISABLE() (peripheral.ACR &= ~FLASH_ACR_DCEN)
#define __HAL_FLASH_DATA_CACHE_RESET() (++cache_resets)
#define __HAL_FLASH_DATA_CACHE_ENABLE() (peripheral.ACR |= FLASH_ACR_DCEN)

typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct { uint32_t TypeErase, Page, NbPages; } FLASH_EraseInitTypeDef;
static struct { uint32_t SR, ACR; } peripheral;
#define FLASH (&peripheral)
#define HSEM ((void *)0)
static unsigned irq_mask, ipsr, stop_disabled, locked, sem2, sem7;
static unsigned busy2, busy6, busy7, clock_ms, nops, since_release, cache_resets;
static unsigned writes, erases, on_calls, off_calls, unlocks, locks, starts;
static unsigned unlock_fail, lock_fail, on_fail, off_fail, bad_readback, bad_page;
static unsigned fail_erase_at, nested, nested_calls, race_flags;
static uint32_t pages[16];
static HAL_StatusTypeDef operation_status;
static bool ZbApp_NvmInit(void);

static uint32_t __get_PRIMASK(void) { return irq_mask; }
static uint32_t __get_IPSR(void) { return ipsr; }
static void __disable_irq(void)
{
    assert(!irq_mask);
    irq_mask = 1;
    if (++starts == 2 && race_flags) peripheral.SR = race_flags;
}
static void __enable_irq(void) { assert(irq_mask); irq_mask = 0; }
static void __NOP(void) { ++nops; ++since_release; }
static uint32_t HAL_GetTick(void)
{
    assert(!irq_mask);
    clock_ms += 100; /* No wall-clock waiting for simulated contention. */
    return clock_ms;
}
static void UTIL_LPM_SetStopMode(uint32_t mask, unsigned state)
{
    assert(mask == (1U << CFG_LPM_NVM_FLASH));
    assert(!irq_mask);
    if (state == UTIL_LPM_DISABLE) { assert(!stop_disabled); stop_disabled = 1; }
    else { assert(stop_disabled); stop_disabled = 0; }
}
static unsigned LL_HSEM_1StepLock(void *unused, unsigned id)
{
    UNUSED(unused);
    assert(stop_disabled);
    if (id == 2) {
        assert(!irq_mask && !sem2);
        if (busy2) return 1;
        sem2 = 1;
    } else {
        assert(id == 7 && irq_mask && sem2 && !sem7);
        assert(since_release >= 64);
        if (busy7) { if (busy7 != UINT32_MAX) --busy7; return 1; }
        assert(!busy6 && !peripheral.SR);
        sem7 = 1;
    }
    return 0;
}
static unsigned LL_HSEM_GetStatus(void *unused, unsigned id)
{
    UNUSED(unused);
    assert(id == 6 && irq_mask && sem2);
    return busy6;
}
static void LL_HSEM_ReleaseLock(void *unused, unsigned id, unsigned process)
{
    UNUSED(unused);
    assert(process == 0);
    if (id == 7) { assert(sem7 && sem2 && irq_mask); sem7 = 0; since_release = 0; }
    else { assert(id == 2 && sem2 && !sem7 && !irq_mask); sem2 = 0; }
}
static HAL_StatusTypeDef HAL_FLASH_Unlock(void)
{
    assert(sem2 && !sem7 && !irq_mask && stop_disabled);
    ++unlocks;
    if (unlock_fail) return HAL_ERROR;
    locked = 0;
    return HAL_OK;
}
static HAL_StatusTypeDef HAL_FLASH_Lock(void)
{
    assert(sem2 && !sem7 && !irq_mask && stop_disabled);
    ++locks;
    locked = 1;
    return lock_fail ? HAL_ERROR : HAL_OK;
}
static unsigned SHCI_C2_FLASH_EraseActivity(unsigned activity)
{
    assert(sem2 && !sem7 && !irq_mask && stop_disabled && !locked);
    if (activity == ERASE_ACTIVITY_ON) ++on_calls;
    else ++off_calls;
    if (nested) {
        ++nested_calls;
        assert(HW_FLASH_Write(FLASH_BASE + CFG_NVM_BASE_ADDRESS, 0) == HW_BUSY);
        assert(sem2 && stop_disabled);
    }
    return (activity ? on_fail : off_fail);
}
static HAL_StatusTypeDef HAL_FLASH_Program(unsigned type, uint32_t address, uint64_t data)
{
    assert(type == FLASH_TYPEPROGRAM_DOUBLEWORD);
    assert(sem2 && sem7 && irq_mask && !locked && stop_disabled);
    assert(!busy6 && !peripheral.SR);
    ++writes;
#if FLASH_EEPROM_INTEGRATION
    assert(*(uint64_t *)(uintptr_t)address == UINT64_MAX);
#endif
    if (operation_status == HAL_OK && !bad_readback) memcpy((void *)(uintptr_t)address, &data, 8);
    return operation_status;
}
static HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *request, uint32_t *page_error)
{
    assert(sem2 && sem7 && irq_mask && !locked && stop_disabled);
    assert(request->TypeErase == FLASH_TYPEERASE_PAGES && request->NbPages == 1);
    assert(on_calls == off_calls + 1);
    pages[erases++ % 16] = request->Page;
    if (fail_erase_at == erases) { *page_error = request->Page; return HAL_ERROR; }
    *page_error = bad_page ? request->Page : UINT32_MAX;
    if (operation_status == HAL_OK && !bad_readback)
        memset((void *)(uintptr_t)(FLASH_BASE + request->Page * FLASH_PAGE_SIZE), 0xff, FLASH_PAGE_SIZE);
    return operation_status;
}

#if !FLASH_EEPROM_INTEGRATION
static int init_status, format_status, init_calls, format_calls;
int EE_Init(int format, uint32_t base)
{
    assert(base == FLASH_BASE + CFG_NVM_BASE_ADDRESS);
    ++init_calls;
    if (format) { ++format_calls; return format_status; }
    return init_status;
}
#endif
#define ZbApp_Log(...) ((void)0)

static void reset(void)
{
    irq_mask = ipsr = stop_disabled = sem2 = sem7 = 0;
    busy2 = busy6 = busy7 = clock_ms = nops = cache_resets = 0;
    writes = erases = on_calls = off_calls = unlocks = locks = starts = 0;
    unlock_fail = lock_fail = on_fail = off_fail = bad_readback = bad_page = 0;
    fail_erase_at = nested = nested_calls = race_flags = 0;
    locked = 1;
    since_release = 0;
    peripheral.SR = 0;
    peripheral.ACR = FLASH_ACR_DCEN;
    operation_status = HAL_OK;
    memset((void *)(uintptr_t)(FLASH_BASE + CFG_NVM_BASE_ADDRESS), 0, CFG_EE_BANK0_SIZE);
}
static void released(void)
{
    assert(!irq_mask && !sem2 && !sem7 && !stop_disabled && locked);
}
#if !FLASH_EEPROM_INTEGRATION
static void reusable(void)
{
    released();
    busy2 = busy6 = busy7 = peripheral.SR = 0;
    unlock_fail = lock_fail = on_fail = off_fail = bad_readback = bad_page = race_flags = 0;
    operation_status = HAL_OK;
    assert(HW_FLASH_Write(FLASH_BASE + CFG_NVM_BASE_ADDRESS + 64, 42) == HW_OK);
    released();
}
#endif

int main(void)
{
    const uint32_t address = FLASH_BASE + CFG_NVM_BASE_ADDRESS;
#if !FLASH_EEPROM_INTEGRATION
    const uint32_t first_page = CFG_NVM_BASE_ADDRESS / FLASH_PAGE_SIZE;
#endif
    assert(VirtualAlloc((void *)(uintptr_t)address, CFG_EE_BANK0_SIZE,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) == (void *)(uintptr_t)address);

#if FLASH_EEPROM_INTEGRATION
    reset();
    memset((void *)(uintptr_t)address, 0xff, CFG_EE_BANK0_SIZE);
    assert(ZbApp_NvmInit()); /* Empty EEPROM can still be formatted. */
    assert(erases == 16);
    for (uint32_t i = 0; i < 20000; ++i) {
        uint16_t slot = (uint16_t)(i % 137);
        uint32_t value = 0;
        assert(EE_Write(0, slot, i + 1) == EE_OK);
        assert(EE_Read(0, slot, &value) == EE_OK && value == i + 1);
        released();
        if (i % 600 == 0) assert(ZbApp_NvmInit()); /* Reconstruct EE state after reboot. */
    }
    assert(erases > 16); /* Actually exercised page transfers and pool cleanup. */
    assert(on_calls == off_calls);
    assert(ZbApp_NvmInit());
    for (uint16_t slot = 0; slot < 137; ++slot) {
        uint32_t value = 0;
        uint32_t latest = 19999U - ((19999U - slot) % 137U);
        assert(EE_Read(0, slot, &value) == EE_OK && value == latest + 1);
    }
    released();
    printf("PASS: actual EEPROM + Flash driver: 20000 updates, reboot recovery, %u page erases\n", erases);
#else
    reset();
    peripheral.SR = FLASH_FLAG_OPTVERR;
    assert(HW_FLASH_Write(address, 0x123456789abcdef0ULL) == HW_OK);
    assert(writes == 1 && cache_resets == 1 && unlocks == 1 && locks == 1);
    assert(peripheral.ACR == FLASH_ACR_DCEN);
    assert(*(uint64_t *)(uintptr_t)address == 0x123456789abcdef0ULL);
    released();

    reset();
    busy7 = 3;
    assert(HW_FLASH_Write(address, 7) == HW_OK);
    assert(writes == 1 && nops >= 256);
    released();

    reset();
    assert(HW_FLASH_Erase(first_page, 3, 1) == HW_OK);
    assert(erases == 3 && on_calls == 1 && off_calls == 1);
    assert(pages[0] == first_page && pages[1] == first_page + 1 && pages[2] == first_page + 2);
    reusable();

    for (unsigned kind = 0; kind < 6; ++kind) {
        reset();
        if (kind == 0) busy2 = 1;
        if (kind == 1) busy6 = 1;
        if (kind == 2) busy7 = UINT32_MAX;
        if (kind >= 3) peripheral.SR = 1U << (kind - 3);
        assert(HW_FLASH_Write(address, 1) == HW_BUSY);
        assert(writes == 0 && clock_ms >= 1000);
        reusable();
    }
    reset();
    race_flags = FLASH_FLAG_PESD;
    assert(HW_FLASH_Write(address, 1) == HW_BUSY); /* Flag appeared after first check. */
    assert(!writes);
    reusable();

    for (unsigned fail = HAL_ERROR; fail <= HAL_TIMEOUT; ++fail) {
        reset(); operation_status = (HAL_StatusTypeDef)fail;
        assert(HW_FLASH_Write(address, 1) == (fail == HAL_TIMEOUT ? HW_TIMEOUT : HW_ERROR));
        reusable();
        reset(); operation_status = (HAL_StatusTypeDef)fail;
        assert(HW_FLASH_Erase(first_page, 3, 0) == (fail == HAL_TIMEOUT ? HW_TIMEOUT : HW_ERROR));
        assert(erases == 1 && off_calls == 1);
        reusable();
    }
    reset(); fail_erase_at = 2;
    assert(HW_FLASH_Erase(first_page, 3, 0) == HW_ERROR);
    assert(erases == 2 && off_calls == 1);
    reusable();

    reset(); busy7 = UINT32_MAX;
    assert(HW_FLASH_Erase(first_page, 2, 0) == HW_BUSY);
    assert(erases == 0 && on_calls == 1 && off_calls == 1);
    reusable();
    for (unsigned kind = 0; kind < 6; ++kind) {
        reset();
        if (kind == 0) unlock_fail = 1;
        if (kind == 1) on_fail = 1;
        if (kind == 2) off_fail = 1;
        if (kind == 3) lock_fail = 1;
        if (kind == 4) bad_readback = 1;
        if (kind == 5) bad_page = 1;
        assert(HW_FLASH_Erase(first_page, 3, 0) == HW_ERROR);
        if (kind == 1) assert(!erases && off_calls == 1);
        reusable();
    }
    reset(); bad_readback = 1;
    assert(HW_FLASH_Write(address, 1) == HW_ERROR);
    reusable();
    reset(); nested = 1;
    assert(HW_FLASH_Erase(first_page, 1, 0) == HW_OK);
    assert(nested_calls == 2 && writes == 0 && erases == 1);
    reusable();

    reset();
    assert(HW_FLASH_Write(address - 8, 1) == HW_ERROR);
    assert(HW_FLASH_Write(address + 1, 1) == HW_ERROR);
    assert(HW_FLASH_Write(address + CFG_EE_BANK0_SIZE, 1) == HW_ERROR);
    assert(HW_FLASH_Erase(first_page - 1, 1, 0) == HW_ERROR);
    assert(HW_FLASH_Erase(first_page, 17, 0) == HW_ERROR);
    assert(HW_FLASH_Erase(first_page, 0, 0) == HW_ERROR);
    assert(!unlocks && !writes && !erases);
    irq_mask = 1;
    assert(HW_FLASH_Write(address, 1) == HW_BUSY && irq_mask == 1);
    irq_mask = 0; ipsr = 1;
    assert(HW_FLASH_Erase(first_page, 1, 0) == HW_BUSY);
    ipsr = 0;
    assert(HW_FLASH_Write(address + CFG_EE_BANK0_SIZE - 8, 1) == HW_OK);
    released();

    for (int code = EE_OK; code <= EE_STATE_ERROR; ++code) {
        init_calls = format_calls = 0; init_status = code; format_status = EE_OK;
        bool ok = ZbApp_NvmInit();
        assert(format_calls == (code == EE_STATE_ERROR));
        assert(init_calls == (code == EE_STATE_ERROR ? 2 : 1));
        assert(ok == (code == EE_OK || code == EE_STATE_ERROR));
    }
    init_status = EE_STATE_ERROR; format_status = EE_ERASE_ERROR;
    assert(!ZbApp_NvmInit());
    puts("PASS: actual Flash driver arbitration, per-page grants, faults, readback, cleanup and NVM init");
#endif
    assert(VirtualFree((void *)(uintptr_t)address, 0, MEM_RELEASE));
    return 0;
}
