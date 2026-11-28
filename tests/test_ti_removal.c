/* Compile the actual TI removal module against fault-injectable NV/RTOS mocks. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include GATEWAY_REMOVAL_SOURCE
static NVINTF_nvFuncts_t api;
static uint8_t disk[3][sizeof(APSME_TCLinkKeyNVEntry_t)];
static uint8_t exists[3], readFail, writeFail, corrupt, childPresent, skipSave;
static uint8_t target[8]={1,2,3,4,5,6,7,8};
static uint8_t mockRead(NVINTF_itemID_t id,uint16_t off,uint16_t n,void *p)
{
 if(readFail) return 1;
 if(id.itemID!=4) { memset(p,0,n); return 0; }
 if(!exists[id.subID]) return 10;
 memcpy(p,disk[id.subID]+off,n);
 if(corrupt) ((uint8_t *)p)[0]^=1;
 return 0;
}
static uint32_t mockLen(NVINTF_itemID_t id)
{ return id.itemID==4 ? (exists[id.subID] ? sizeof(disk[0]) : 0) : 4; }
static uint8_t mockUpdate(NVINTF_itemID_t id,uint32_t n,void *p)
{ if(writeFail) return 1; if(id.itemID==4) memcpy(disk[id.subID],p,n); return 0; }
static uint8_t mockWrite(NVINTF_itemID_t id,uint16_t n,void *p) { return mockUpdate(id,n,p); }
static uint8_t mockDelete(NVINTF_itemID_t id) { exists[id.subID]=0; return 0; }
unsigned char ZDSecMgrDeviceRemoveByExtAddr(unsigned char *ieee) { (void)ieee; return ZNwkUnknownDevice; }
void *AssocGetWithExt(unsigned char *ieee) { (void)ieee; return childPresent ? target : NULL; }
void ZDApp_SaveNetworkStateEvt(void)
{ uint32_t zero=0; if(!skipSave) api.updateItem((NVINTF_itemID_t){1,0,1},4,&zero); }
static void resetTest(void)
{
 memset(disk,0,sizeof(disk)); memset(exists,0,sizeof(exists));
 readFail=writeFail=corrupt=childPresent=skipSave=0;
 installed=bootFault=fault=0; owner=NULL; head=tail=0;
 api=(NVINTF_nvFuncts_t){mockUpdate,mockUpdate,mockWrite,mockDelete,mockRead,mockLen};
 GatewayRemoval_InstallNV(&api);
}
static GatewayRemovalResult run(void)
{ GatewayRemovalResult r; GatewayRemoval_Begin(); GatewayRemoval_End(target,0x1234,1); assert(GatewayRemoval_Pop(&r)); return r; }
int main(void)
{
 resetTest(); exists[0]=1; memcpy(disk[0],target,8);
 GatewayRemovalResult r=run(); assert(!r.tc && !r.app && !r.nv); assert(!memcmp(disk[0],(uint8_t[24]){0},24));
 resetTest(); r=run(); assert(!r.tc && !r.nv); /* missing TC entries are valid */
 resetTest(); readFail=1; r=run(); assert(r.tc && r.nv);
 resetTest(); writeFail=1; r=run(); assert(r.nv); writeFail=0; r=run(); assert(r.nv); /* sticky */
 resetTest(); exists[0]=1; memcpy(disk[0],target,8);
 GatewayRemoval_Begin(); corrupt=1; uint8_t zero[24]={0}; api.updateItem((NVINTF_itemID_t){1,4,0},24,zero);
 corrupt=0; GatewayRemoval_End(target,0x1234,2); assert(GatewayRemoval_Pop(&r) && r.nv);
 resetTest(); childPresent=1; r=run(); assert(r.nv);
 resetTest(); skipSave=1; r=run(); assert(r.nv); /* no write is not verified persistence */
 resetTest(); GatewayRemoval_Failed(target,0x1234,0xE9); assert(GatewayRemoval_Pop(&r) && r.status==0xE9 && r.nv);
 resetTest(); installed=0; r=run(); assert(r.nv);
 puts("TI removal: 9 fault-injection scenarios passed");
 return 0;
}
