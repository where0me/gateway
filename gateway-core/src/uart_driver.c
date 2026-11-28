#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <string.h>
#include "uart_driver.h"

int uart_init(const char *device, int baudrate) {
    int fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd == -1) {
        perror("[UART] 无法打开串口设备");
        return -1;
    }
    if (fcntl(fd, F_SETFL, 0) != 0) goto fail; // 恢复为阻塞状态

    struct termios options;
    if (tcgetattr(fd, &options) != 0) goto fail;

    // 支持常用波特率配置
    speed_t br = B115200;
    if (baudrate == 9600) {
        br = B9600;
    } else if (baudrate == 115200) {
        br = B115200;
    } else if (baudrate == 921600) {
        br = B921600;
    } else {
        errno = EINVAL;
        goto fail;
    }

    if (cfsetispeed(&options, br) != 0 || cfsetospeed(&options, br) != 0) goto fail;

    // 配置 8N1 原始模式，确保 16 进制物理字节流不被 Linux 终端驱动篡改
    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;
#ifdef CRTSCTS
    options.c_cflag &= ~CRTSCTS;
#endif
    options.c_cflag |= (CLOCAL | CREAD);
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHONL | ISIG | IEXTEN);
    options.c_oflag &= ~OPOST;
    options.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | INLCR | IGNCR | ICRNL |
                         INPCK | ISTRIP | IXON | IXOFF | IXANY);

    /* A timed read lets the receiver detect a lost final fragment without
     * requiring a normal block-end query from the node. */
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &options) != 0) goto fail;
    struct termios applied;
    if (tcgetattr(fd, &applied) != 0) goto fail;
    if (cfgetispeed(&applied) != br || cfgetospeed(&applied) != br) {
        errno = EIO;
        goto fail;
    }
    printf("[UART] 串口 %s 初始化成功，波特率: %d\n", device, baudrate);
    return fd;
fail:
    perror("[UART] 串口参数设置失败");
    close(fd);
    return -1;
}

int mac_string_to_bytes(const char *mac_str, unsigned char *mac_bin) {
    unsigned int bytes[8];
    // sscanf 可以完美兼容大写和小写的十六进制解析
    int filled = sscanf(mac_str, "%x:%x:%x:%x:%x:%x:%x:%x",
                        &bytes[0], &bytes[1], &bytes[2], &bytes[3],
                        &bytes[4], &bytes[5], &bytes[6], &bytes[7]);
    if (filled != 8) return -1;
    for (int i = 0; i < 8; i++) mac_bin[i] = (unsigned char)bytes[i];
    return 0;
}


// 纯正方案B组包：[FE] [CMD] [LEN] [Payload] [BCC]
int send_hex_frame_to_uart_checked(int fd, unsigned char cmd, unsigned char len, const unsigned char *payload)
{
    if (fd < 0 || (len && !payload)) return -1;

    // 总长度 = 帧头(1) + CMD(1) + LEN(1) + 载荷长度 + BCC(1)
    int frame_len = 3 + len + 1; 
    unsigned char *frame = (unsigned char *)malloc(frame_len);
    if (!frame) return -1;

    frame[0] = 0xFE;       // SOF
    frame[1] = cmd;        // CMD
    frame[2] = len;        // LEN

    unsigned char bcc = cmd ^ len; // BCC 从 CMD 开始异或

    // 拷贝 Payload 并计算 BCC
    for (int i = 0; i < len; i++) {
        frame[3 + i] = payload[i];
        bcc ^= payload[i];
    }

    frame[3 + len] = bcc;  // 尾部填入 BCC

    int sent = 0;
    while (sent < frame_len)
    {
        ssize_t written = write(fd, frame + sent, (size_t)(frame_len - sent));
        if (written < 0)
        {
            if (errno == EINTR) continue;
            perror("[UART] 串口帧发送失败");
            break;
        }
        if (written == 0)
        {
            fprintf(stderr, "[UART] 串口帧发送中断，仅发送 %d/%d 字节\n", sent, frame_len);
            break;
        }
        sent += (int)written;
    }
    free(frame);
    return sent == frame_len ? 0 : -1;
}

void send_hex_frame_to_uart(int fd, unsigned char cmd, unsigned char len, const unsigned char *payload)
{
    (void)send_hex_frame_to_uart_checked(fd,cmd,len,payload);
}
