#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define USART1 ((void*)1)
typedef struct {void *Instance;} UART_HandleTypeDef;
static UART_HandleTypeDef huart1={USART1};
#define hw_uart1 0
#define CFG_TASK_NODE_CONSOLE 21
#define CFG_SCH_PRIO_1 1
#define UTIL_SEQ_RFU 0
static unsigned calls,in_irq,armed,scheduled;
static unsigned rx_enabled,error_enabled,hal_irqs;
static uint8_t fifo[8];static unsigned fifo_read,fifo_count,error_flags;
static char output[4096];
static void APP_ZIGBEE_CommTest(void){assert(!in_irq);calls++;}
static unsigned LL_USART_IsActiveFlag_RXNE_RXFNE(void *u){assert(u==USART1);return fifo_read<fifo_count;}
static uint8_t LL_USART_ReceiveData8(void *u){assert(u==USART1 && fifo_read<fifo_count);return fifo[fifo_read++];}
static void LL_USART_EnableIT_RXNE_RXFNE(void *u){assert(u==USART1);armed++;rx_enabled=1;}
static void LL_USART_EnableIT_ERROR(void *u){assert(u==USART1);armed++;error_enabled=1;}
static void LL_USART_DisableIT_RXNE_RXFNE(void *u){assert(u==USART1);rx_enabled=0;}
static void LL_USART_DisableIT_ERROR(void *u){assert(u==USART1);error_enabled=0;}
static void HAL_UART_IRQHandler(UART_HandleTypeDef *u){assert(u==&huart1 && in_irq && !rx_enabled && !error_enabled);hal_irqs++;}
#define ERR_FN(name,bit) static unsigned LL_USART_IsActiveFlag_##name(void *u){assert(u==USART1);return error_flags&(bit);} \
static void LL_USART_ClearFlag_##name(void *u){assert(u==USART1);error_flags&=~(bit);}
ERR_FN(ORE,1U) ERR_FN(FE,2U) ERR_FN(NE,4U) ERR_FN(PE,8U)
static void UTIL_SEQ_RegTask(unsigned task,unsigned priority,void (*fn)(void)){(void)task;(void)priority;(void)fn;}
static void UTIL_SEQ_SetTask(unsigned task,unsigned priority){assert(task==(1U<<21) && priority==1);scheduled++;}
static uint32_t __get_PRIMASK(void){return 0;}static void __disable_irq(void){}
static void __set_PRIMASK(uint32_t m){assert(!m);}
static int HAL_UART_Transmit(UART_HandleTypeDef *u,uint8_t *p,unsigned n,unsigned timeout)
{assert(u==&huart1 && p && n && n<192 && timeout==100 && !in_irq);
 assert(strlen(output)+n<sizeof(output));strncat(output,(const char *)p,n);return 0;}
/* Actual node_console.h is appended by the runner, with platform includes removed. */
static void feed(const char *text,int drain);
int main(void);
