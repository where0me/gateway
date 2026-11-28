/* Custom CC1352R coordinator application entry point.
 * TI SDK startup/stack are retained; no LaunchPad button or LED dependency.
 */
#include "gateway_bridge.h"
void sampleApp_task(NVINTF_nvFuncts_t *pfnNV)
{
    GatewayBridge_Run(pfnNV);
}
