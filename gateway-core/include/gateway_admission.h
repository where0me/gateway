#ifndef GATEWAY_ADMISSION_H
#define GATEWAY_ADMISSION_H
static int submit_removed_device_leave(const unsigned char mac[8])
{
    pthread_mutex_lock(&g_uart_mutex);
    int submitted = g_uart_fd >= 0 &&
        send_hex_frame_to_uart_checked(g_uart_fd, 0x11, 8, mac) == 0;
    pthread_mutex_unlock(&g_uart_mutex);
    return submitted;
}

static void accept_verified_scan_node(const unsigned char *payload,uint8_t lqi,int8_t rssi)
{
    (void)lqi; (void)rssi; /* Cached neighbor values are not fresh signal samples. */
    char mac[24],address[8];
    unsigned short_addr=payload[8]|((unsigned)payload[9]<<8);
    unsigned any=0,all=255;
    for(unsigned i=0;i<8;i++){any|=payload[i];all&=payload[i];}
    if(!any || all==255 || short_addr==0 || short_addr>=0xfff8) return;
    snprintf(mac,sizeof(mac),"%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
        payload[0],payload[1],payload[2],payload[3],payload[4],payload[5],payload[6],payload[7]);
    if(is_device_in_db(mac))return;
    RevokedNodeRecord_t revoked;
    if(get_revoked_node_by_mac(mac,&revoked))return;
    if(admission_is_closed()) {
        /* Catch an announcement already in flight when the user closed join.
         * Persist the removal before sending, so later uplinks retry it. */
        if(queue_revoked_node(mac,(uint16_t)short_addr)==0)
            (void)submit_removed_device_leave(payload);
        return;
    }
    if(insert_new_device(mac,"Unknown Device")!=0) {
        gateway_log_event("ERROR","backend","node_register_failed",mac,short_addr,-1,"节点入网，但数据库登记失败"); return;
    }
    snprintf(address,sizeof(address),"%04X",short_addr);update_device_online(mac,address);
    (void)save_device_interview(mac,"{\"state\":\"interviewing\"}");
    gateway_log_event("DEBUG","coordinator","node_announced_pending",mac,short_addr,-1,
        "收到设备宣告，仅建立内部地址记录；等待入网后的应用上行，暂不显示节点");
}
#endif
