#include <stdint.h>
typedef struct { uint8_t systemID; uint16_t itemID,subID; } NVINTF_itemID_t;
typedef struct {
 uint8_t (*createItem)(NVINTF_itemID_t,uint32_t,void *);
 uint8_t (*updateItem)(NVINTF_itemID_t,uint32_t,void *);
 uint8_t (*writeItem)(NVINTF_itemID_t,uint16_t,void *);
 uint8_t (*deleteItem)(NVINTF_itemID_t);
 uint8_t (*readItem)(NVINTF_itemID_t,uint16_t,uint16_t,void *);
 uint32_t (*getItemLen)(NVINTF_itemID_t);
} NVINTF_nvFuncts_t;
#define NVINTF_SUCCESS 0
#define NVINTF_FAILURE 1
#define NVINTF_NOTFOUND 10
#define NVINTF_SYSID_ZSTACK 1
