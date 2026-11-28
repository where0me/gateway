#include "gateway_removal.h"
#include "zcomdef.h"
#include "aps_mede.h"
#include "zd_sec_mgr.h"
#include "assoc_list.h"
#include <string.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/hal/Hwi.h>

extern void ZDApp_SaveNetworkStateEvt(void);
static NVINTF_nvFuncts_t original;
static Task_Handle owner;
static uint8_t fault, installed;
static unsigned writes;
/* A failed write can already have invalidated a RAM security entry. Do not
 * declare a later duplicate Leave successful in this boot after that failure. */
static uint8_t bootFault;
static GatewayRemovalResult results[16];
static volatile unsigned head, tail;

static bool auditing(void) { return owner && owner == Task_self(); }
static uint8_t verify(NVINTF_itemID_t id, uint32_t len, const void *data, uint8_t status)
{
    if (!auditing()) return status;
    ++writes;
    if (status != NVINTF_SUCCESS || !data || len > 65535U ||
        original.getItemLen(id) != len) {
        fault = 1; return status ? status : NVINTF_FAILURE;
    }
    uint8_t buf[32];
    for (uint32_t offset=0; offset<len; offset+=sizeof(buf)) {
        uint16_t count=(len-offset)>sizeof(buf) ? sizeof(buf) : len-offset;
        if (original.readItem(id, offset, count, buf) != NVINTF_SUCCESS ||
            memcmp(buf, (const uint8_t *)data+offset, count)) {
            fault=1; return NVINTF_FAILURE;
        }
    }
    return status;
}
static uint8_t update(NVINTF_itemID_t id, uint32_t len, void *data)
{ return verify(id,len,data,original.updateItem(id,len,data)); }
static uint8_t create(NVINTF_itemID_t id, uint32_t len, void *data)
{ return verify(id,len,data,original.createItem(id,len,data)); }
static uint8_t write(NVINTF_itemID_t id, uint16_t len, void *data)
{ return verify(id,len,data,original.writeItem(id,len,data)); }
static uint8_t eraseItem(NVINTF_itemID_t id)
{
    uint8_t status=original.deleteItem(id);
    if (auditing()) {
        uint8_t byte;
        ++writes;
        if ((status != NVINTF_SUCCESS && status != NVINTF_NOTFOUND) ||
            original.getItemLen(id) != 0 ||
            original.readItem(id,0,1,&byte) != NVINTF_NOTFOUND) fault=1;
    }
    return status;
}
void GatewayRemoval_InstallNV(NVINTF_nvFuncts_t *api)
{
    if (!api || installed || !api->updateItem || !api->createItem ||
        !api->writeItem || !api->deleteItem || !api->readItem || !api->getItemLen) return;
    original=*api;
    api->updateItem=update; api->createItem=create;
    api->writeItem=write; api->deleteItem=eraseItem;
    installed=1;
}
void GatewayRemoval_Begin(void)
{
    fault=bootFault || !installed; writes=0; owner=Task_self();
}
static void publish(const GatewayRemovalResult *r)
{
    UInt key=Hwi_disable();
    unsigned next=(head+1)%16;
    /* Losing a result must never produce success. Backend keeps its durable
     * task in that case; at most eight removals are admitted by the bridge. */
    if (next != tail) { results[head]=*r; head=next; }
    Hwi_restore(key);
}
void GatewayRemoval_End(const uint8_t ieee[8], uint16_t address, uint8_t evidence)
{
    GatewayRemovalResult r={0};
    memcpy(r.ieee,ieee,8); r.address=address; r.evidence=evidence;
    r.tc=0xFF; r.app=0xFF; r.nv=0xFF;
    if (installed) {
        r.tc=0;
        /* Inspect every slot. Missing and unreadable are different; never
         * interpret a read failure as proof that a key is absent. */
        for (unsigned i=0; i<gZDSECMGR_TC_DEVICE_MAX; ++i) {
            NVINTF_itemID_t id={NVINTF_SYSID_ZSTACK,ZCD_NV_EX_TCLK_TABLE,i};
            APSME_TCLinkKeyNVEntry_t entry;
            uint8_t status=original.readItem(id,0,sizeof(entry),&entry);
            if (status==NVINTF_NOTFOUND) continue;
            if (status!=NVINTF_SUCCESS) { r.tc=status; fault=1; continue; }
            if (!memcmp(entry.extAddr,ieee,8)) {
                memset(&entry,0,sizeof(entry));
                status=update(id,sizeof(entry),&entry);
                if (status) r.tc=status;
            }
        }
        /* Official LeaveUpdate has removed APS entries; its NV writes were
         * audited from BEFORE that call, including ignored write errors. */
        uint8_t status=ZDSecMgrDeviceRemoveByExtAddr((uint8_t *)ieee);
        r.app=(status==ZSuccess || status==ZNwkUnknownDevice) ? 0 : status;
        if (AssocGetWithExt((uint8_t *)ieee)!=NULL) fault=1;
        ZDApp_SaveNetworkStateEvt();
        r.nv=(fault || !writes) ? NVINTF_FAILURE : NVINTF_SUCCESS;
    }
    bootFault |= fault;
    owner=NULL;
    publish(&r);
}
void GatewayRemoval_Failed(const uint8_t ieee[8], uint16_t address, uint8_t status)
{
    GatewayRemovalResult r={0}; memcpy(r.ieee,ieee,8); r.address=address;
    r.tc=r.app=r.nv=0xFF; r.evidence=1; r.status=status;
    publish(&r);
}
bool GatewayRemoval_Pop(GatewayRemovalResult *r)
{
    UInt key=Hwi_disable();
    bool available=tail!=head;
    if (available) { *r=results[tail]; tail=(tail+1)%16; }
    Hwi_restore(key); return available;
}
