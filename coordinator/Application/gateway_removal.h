#ifndef GATEWAY_REMOVAL_H
#define GATEWAY_REMOVAL_H
#include <stdint.h>
#include <stdbool.h>
#include <ti/common/nv/nvintf.h>
/* Install before stackTask_init; never bypass the TI NV driver. */
void GatewayRemoval_InstallNV(NVINTF_nvFuncts_t *api);
/* Stack-task only, surrounding the official ZDApp_LeaveUpdate path. */
void GatewayRemoval_Begin(void);
void GatewayRemoval_End(const uint8_t ieee[8], uint16_t address, uint8_t evidence);
void GatewayRemoval_Failed(const uint8_t ieee[8], uint16_t address, uint8_t status);
/* Single stack-task producer, application-task consumer. No UART in stack. */
typedef struct {
    uint8_t ieee[8];
    uint16_t address;
    uint8_t tc, app, nv, evidence, status;
} GatewayRemovalResult;
bool GatewayRemoval_Pop(GatewayRemovalResult *result);
#endif
