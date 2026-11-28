#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "download_reset.h"
#include "zigbee_vibration_store.h"
#include "qspi_nor.h"

static uint8_t external_flash[8U * 1024U * 1024U];
static uint64_t marker;
static unsigned erases, invalidations, reads;
static unsigned starts, commits;
static int fail_init, fail_write, fail_read, corrupt_read, fail_commit;
static unsigned cut_after;
static uint64_t read_marker(void *context) { (void)context; return marker; }
uint32_t HAL_GetTick(void) { return 0; }
HAL_StatusTypeDef QspiNor_Init(uint32_t *id, uint32_t *bytes)
{
    if (id) *id=0xc22817;
    if (bytes) *bytes=sizeof(external_flash);
    return fail_init ? HAL_ERROR : HAL_OK;
}
HAL_StatusTypeDef QspiNor_EraseRange(uint32_t address, uint32_t bytes)
{
    uint32_t end=(address+bytes+4095U)&~4095U; address&=~4095U;
    assert(end<=sizeof(external_flash));
    memset(external_flash+address,255,end-address); erases++; return HAL_OK;
}
HAL_StatusTypeDef QspiNor_Read(uint32_t address, uint8_t *data, uint32_t bytes)
{
    assert(address+bytes<=sizeof(external_flash)); reads++;
    if (fail_read) return HAL_ERROR;
    memcpy(data, external_flash + address, bytes);
    if (corrupt_read && bytes==4) data[0]^=1;
    return HAL_OK;
}
HAL_StatusTypeDef QspiNor_Write(uint32_t address, const uint8_t *data, uint32_t bytes)
{
    assert(address+bytes<=sizeof(external_flash));
    if (fail_write || (cut_after && invalidations>=cut_after)) return HAL_ERROR;
    for (unsigned i=0;i<bytes;i++) {
        assert((external_flash[address+i]&data[i])==data[i]);
        external_flash[address+i]&=data[i];
    }
    if (bytes==4 && !memcmp(data,"\0\0\0\0",4)) invalidations++;
    return HAL_OK;
}
HAL_StatusTypeDef QspiNor_ProgramPageAsync(uint32_t a,const uint8_t *p,uint16_t n) { return QspiNor_Write(a,p,n); }
HAL_StatusTypeDef QspiNor_ProgramService(void) { return HAL_OK; }
void QspiNor_ProgramAbort(void) { }
static int discard_records(void *context)
{
    (void)context; starts++;
    unsigned before=erases;
    int result=ZigbeeVibrationStore_ResetRecords()==HAL_OK;
    assert(erases==before); /* Real production reset must not erase any payload. */
    return result;
}
static int commit_marker(void *context)
{
    (void)context;
    uint8_t descriptor[V2_BATCH_LEN];
    assert(!ZigbeeVibrationStore_Pending(descriptor) && ZigbeeVibrationStore_FreeSlots()==16);
    commits++;
    if (fail_commit) { marker = UINT64_MAX; return 0; } /* Power loss during marker commit. */
    marker = DOWNLOAD_RESET_DONE; return 1;
}
static void reset_fixture(void)
{
    marker = DOWNLOAD_RESET_ARMED;
    invalidations = reads = starts = commits = erases = cut_after = 0;
    fail_init = fail_write = fail_read = corrupt_read = fail_commit = 0;
    memset(external_flash, 0x5a, sizeof(external_flash));
}
static void reset_io_counters(void) { invalidations = reads = starts = commits = 0; }
static const DownloadResetIO io = {NULL, read_marker, discard_records, commit_marker};
static void capture(uint32_t batch)
{
    assert(ZigbeeVibrationStore_Begin(26667,500,batch)==HAL_OK);
    int16_t samples[160][3]={{123,456,789}};
    for(unsigned first=0;first<13334;) {
        unsigned n=13334-first;if(n>160)n=160;
        assert(ZigbeeVibrationStore_Append(samples,n)==HAL_OK);first+=n;
    }
    assert(ZigbeeVibrationStore_Finish(NULL)==HAL_OK);
}

#ifdef DOWNLOAD_PLATFORM_TEST
/* Compile the actual target marker-commit function against peripheral stubs.
 * Its page/address limits protect Zigbee NVM and executable code. */
typedef struct { uint32_t TypeErase, Page, NbPages; } FLASH_EraseInitTypeDef;
typedef struct { uint32_t CR4; } PowerRegisters;
typedef struct { uint32_t ACR; } FlashRegisters;
static PowerRegisters power;
static FlashRegisters flash_registers;
#define PWR (&power)
#define FLASH (&flash_registers)
#define PWR_CR4_C2BOOT 1U
#define FLASH_ACR_DCEN 1U
#define FLASH_PAGE_SIZE 4096U
#define FLASH_BASE UINT32_C(0x08000000)
#define FLASH_TYPEERASE_PAGES 2U
#define FLASH_TYPEPROGRAM_DOUBLEWORD 3U
#define FLASH_FLAG_OPTVERR 4U
#define DownloadResetImage (*(uint64_t *)(uintptr_t)DOWNLOAD_RESET_ADDRESS)
#define READ_BIT(reg, mask) ((reg) & (mask))
#define UNUSED(x) ((void)(x))
static unsigned unlocks, locks, page_erases, programs, cache_resets;
static int fail_unlock, fail_lock, fail_page_erase, page_error_only, fail_program;
#define __HAL_FLASH_CLEAR_FLAG(flag) assert((flag) == FLASH_FLAG_OPTVERR)
#define __HAL_FLASH_DATA_CACHE_DISABLE() ((void)0)
#define __HAL_FLASH_DATA_CACHE_RESET() (++cache_resets)
#define __HAL_FLASH_DATA_CACHE_ENABLE() ((void)0)
static HAL_StatusTypeDef HAL_FLASH_Unlock(void) { unlocks++; return fail_unlock ? HAL_ERROR : HAL_OK; }
static HAL_StatusTypeDef HAL_FLASH_Lock(void) { locks++; return fail_lock ? HAL_ERROR : HAL_OK; }
static HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *request, uint32_t *error)
{
    assert(request->TypeErase == FLASH_TYPEERASE_PAGES);
    assert(request->Page == 111U && request->NbPages == 1U);
    page_erases++;
    if (fail_page_erase) return HAL_ERROR;
    if (page_error_only) { *error = request->Page; return HAL_OK; }
    marker = UINT64_MAX; return HAL_OK;
}
static HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uint32_t address, uint64_t data)
{
    assert(type == FLASH_TYPEPROGRAM_DOUBLEWORD && address == DOWNLOAD_RESET_ADDRESS);
    assert(marker == UINT64_MAX && data == DOWNLOAD_RESET_DONE); /* Erase before program, including ECC. */
    programs++; if (fail_program) return HAL_ERROR;
    marker = data; return HAL_OK;
}
static int DownloadReset_Commit(void *context);
static void test_platform(void)
{
    marker = DOWNLOAD_RESET_ARMED; flash_registers.ACR = FLASH_ACR_DCEN;
    assert(DownloadReset_Commit(NULL));
    assert(marker == DOWNLOAD_RESET_DONE && unlocks == 1 && locks == 1);
    assert(page_erases == 1 && programs == 1 && cache_resets == 1);
    power.CR4 = PWR_CR4_C2BOOT;
    assert(!DownloadReset_Commit(NULL) && unlocks == 1); power.CR4 = 0;
    fail_unlock = 1; assert(!DownloadReset_Commit(NULL)); fail_unlock = 0;
    unsigned previous_programs = programs, previous_locks = locks;
    fail_page_erase = 1; assert(!DownloadReset_Commit(NULL)); fail_page_erase = 0;
    assert(programs == previous_programs && locks == previous_locks + 1);
    page_error_only = 1; assert(!DownloadReset_Commit(NULL)); page_error_only = 0;
    assert(programs == previous_programs);
    fail_program = 1; assert(!DownloadReset_Commit(NULL)); fail_program = 0;
    assert(marker == UINT64_MAX);
    fail_lock = 1; assert(!DownloadReset_Commit(NULL)); fail_lock = 0;
    assert(DownloadReset_Commit(NULL));
    puts("actual marker commit: dedicated page, CPU2 exclusion, HAL failures and ECC-safe rewrite PASS");
}
#endif

int main(void)
{
    reset_fixture();
    assert(ZigbeeVibrationStore_Init(NULL,NULL)==HAL_OK);
    capture(1); capture(2);
    uint8_t saved_waveform[6]; memcpy(saved_waveform,external_flash+4096,6);
    unsigned before=erases;
    assert(download_reset_run(&io) == 1);
    assert(erases==before && invalidations==16 && commits==1);
    assert(!memcmp(saved_waveform,external_flash+4096,6));
    assert(external_flash[sizeof(external_flash)-1]==0x5a); /* Outside all slots untouched. */
    uint8_t descriptor[V2_BATCH_LEN];
    assert(ZigbeeVibrationStore_Init(NULL,NULL)==HAL_OK && !ZigbeeVibrationStore_Pending(descriptor));
    assert(ZigbeeVibrationStore_FreeSlots()==16);
    capture(3); assert(erases==before+1); /* Erase only when a slot is used. */
    reset_io_counters();
    for (unsigned boot = 0; boot < 20; ++boot) assert(download_reset_run(&io) == 0);
    assert(!starts && !invalidations && !commits && erases==before+1);
    assert(ZigbeeVibrationStore_Init(NULL,NULL)==HAL_OK && ZigbeeVibrationStore_DescribeBatch(3,descriptor));
    /* Re-download the exact same image: only its marker is restored, no version change. */
    marker = DOWNLOAD_RESET_ARMED;
    assert(download_reset_run(&io) == 1 && erases==before+1);
    assert(!ZigbeeVibrationStore_DescribeBatch(3,descriptor));
    assert(!memcmp(saved_waveform,external_flash+4096,6));
    reset_fixture(); fail_init = 1; assert(download_reset_run(&io) == -1 && !commits && !erases);
    reset_fixture(); fail_write = 1; assert(download_reset_run(&io) == -1 && !commits && !erases);
    fail_write=0; assert(download_reset_run(&io)==1);
    reset_fixture(); fail_read = 1; assert(download_reset_run(&io) == -1 && !commits);
    reset_fixture(); corrupt_read = 1; assert(download_reset_run(&io) == -1 && !commits);
    reset_fixture(); cut_after=5; assert(download_reset_run(&io)==-1 && !commits && invalidations==5);
    assert(marker==DOWNLOAD_RESET_ARMED);
    cut_after=0;reset_io_counters();assert(download_reset_run(&io)==1 && invalidations==16);
    reset_fixture(); fail_commit = 1; assert(download_reset_run(&io) == -1);
    assert(marker == UINT64_MAX);
    fail_commit = 0; reset_io_counters(); assert(download_reset_run(&io) == 1);
    /* Interrupted/unknown marker is never interpreted as cleanup completed. */
    reset_fixture(); marker = UINT64_C(0x1234); assert(download_reset_run(&io) == 1);
    puts("logical download cleanup: actual 16-slot invalidation, payload retained, zero erases, reboot/reload, lazy erase, interruption/faults PASS");
#ifdef DOWNLOAD_PLATFORM_TEST
    test_platform();
#endif
    return 0;
}
