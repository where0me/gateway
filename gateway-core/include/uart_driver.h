#ifndef UART_DRIVER_H
#define UART_DRIVER_H

// 初始化串口，返回文件描述符 fd
int uart_init(const char *device, int baudrate);

// 将 MAC 地址字符串转换成 8 字节数组
int mac_string_to_bytes(const char *mac_str, unsigned char *mac_bin);

// 向串口发送 16 进制协议帧
void send_hex_frame_to_uart(int fd, unsigned char cmd, unsigned char len, const unsigned char *payload);

// 0: complete frame written to the local serial driver; not peer receipt.
int send_hex_frame_to_uart_checked(int fd,unsigned char cmd,unsigned char len,const unsigned char *payload);

#endif