#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
static unsigned calls,mode,copied;static uint8_t wire[300];
static ssize_t fake_write(int fd,const void *p,size_t n){
    assert(fd==3);calls++;
    if(mode==1 && calls==1){errno=EINTR;return -1;}
    if(mode==2 && calls==2){errno=EIO;return -1;}
    if(mode==3)return 0;
    size_t size=n>2?2:n;memcpy(wire+copied,p,size);copied+=(unsigned)size;return (ssize_t)size;
}
#define write fake_write
static int send_hex_frame_to_uart_checked(int,unsigned char,unsigned char,const unsigned char *);
static void send_hex_frame_to_uart(int,unsigned char,unsigned char,const unsigned char *);
int main(void){
    uint8_t p[3]={1,2,3};
    assert(send_hex_frame_to_uart_checked(-1,0x0D,3,p)==-1 && !calls);
    assert(send_hex_frame_to_uart_checked(3,0x0D,3,NULL)==-1 && !calls);
    mode=1;assert(!send_hex_frame_to_uart_checked(3,0x0D,3,p) && copied==7);
    assert(wire[0]==0xFE && wire[1]==0x0D && wire[2]==3 && wire[6]==(0x0D^3^1^2^3));
    calls=copied=0;mode=2;assert(send_hex_frame_to_uart_checked(3,0x0D,3,p)==-1 && copied==2);
    calls=copied=0;mode=3;assert(send_hex_frame_to_uart_checked(3,0x0D,3,p)==-1 && !copied);
    calls=copied=0;mode=0;send_hex_frame_to_uart(3,0x0D,3,p);assert(copied==7);
    puts("UART checked PASS: partial writes/EINTR preserve framing; zero/error/bad descriptors fail explicitly; legacy wrapper unchanged");
}
