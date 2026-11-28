#ifndef RADIO_DIAG_H
#define RADIO_DIAG_H
#include "radio_diag_protocol.h"
#include <stdio.h>
static inline const char *radio_ti_status(unsigned status)
{
    switch(status) {
    case 0:return "成功";
    case 0x10:return "提交内存或资源不足";
    case 0xB7:return "APS未收到确认";
    case 0xE1:return "MAC信道接入失败";
    case 0xE9:return "MAC未收到确认";
    case 0xF0:return "MAC间接事务过期或被清理；不能据此判定是否收到轮询";
    case 0xF1:return "MAC事务队列满";
    default:return "其他协议栈状态";
    }
}
static inline const char *radio_ack_kind(unsigned kind)
{
    switch(kind){case 1:return "批次许可";case 2:return "窗口描述确认";case 3:return "窗口校验结果";
    case 4:return "整批波形确认";case 5:return "振动特征确认";case 6:return "传感器数据校验";
    case 7:return "上传许可续期";case 8:return "批次已完成";default:return "未知回复";}
}
static inline const char *radio_ack_result(unsigned result)
{
    switch(result){case 0:return "通过";case 1:return "缺片补传";case 2:return "重申批次";
    case 3:return "拒绝";case 4:return "存储失败";case 5:return "重发窗口描述";
    case 6:return "窗口已完成";case 7:return "排队等待";default:return "未知";}
}
typedef struct {uint32_t tick,batch;uint16_t address;const char *event,*level;int persist;} RadioDiagLog;
/* Stateless decoding: diagnostic messages never change device/queue state. */
static inline int radio_diag_format(const uint8_t *p,size_t n,char *out,size_t capacity,RadioDiagLog *log)
{
    if(!p || !out || !log || n!=RADIO_DIAG_LEN || p[0]!=2U || p[1]<1U || p[1]>6U)return 0;
    memset(log,0,sizeof(*log));log->tick=radio_u32(p+2);log->address=radio_u16(p+6);
    log->batch=radio_u32(p+12);log->level="DEBUG";
    char polls[128];uint32_t age=radio_u32(p+28);
    if(age==UINT32_MAX)snprintf(polls,sizeof(polls),"累计观测轮询=%u | 最近轮询=未观测（不等于未轮询）",radio_u32(p+24));
    else snprintf(polls,sizeof(polls),"累计观测轮询=%u | 距最近观测轮询=%u ms",radio_u32(p+24),age);
    switch(p[1]) {
    case RADIO_DIAG_SUBMIT:case RADIO_DIAG_AF_RESULT: {
        char window[16]="-",af[16]="未知",seq[16]="未知";
        if(p[10]==2 || p[10]==3)snprintf(window,sizeof(window),"%u",(unsigned)radio_u16(p+16)+1U);
        if(p[19]&RADIO_DIAG_AF_ID_VALID){snprintf(af,sizeof(af),"%u",p[8]);snprintf(seq,sizeof(seq),"%u",p[9]);}
        log->event=p[1]==RADIO_DIAG_SUBMIT?"upload_ack_submit":"upload_ack_result";
        if(p[18]){log->level="WARN";log->persist=1;}
        snprintf(out,capacity,"上传回复%s%s | 短地址=0x%04X | 批次=%u | 窗口=%s | 类型=%s（编号%u）| 回复=%s（0x%02X）| AF事务=%s | ZCL序号=%s | 状态=%s（0x%02X）| 耗时=%u ms | %s%s",
            p[1]==RADIO_DIAG_SUBMIT?"提交":"AF发送结果",p[18]?"失败":"成功",log->address,log->batch,window,radio_ack_kind(p[10]),p[10],
            radio_ack_result(p[11]),p[11],af,seq,radio_ti_status(p[18]),p[18],radio_u32(p+20),polls,
            p[1]==RADIO_DIAG_SUBMIT && !p[18]?" | 提交成功不代表已发射或已投递":"");
        break;
    }
    case RADIO_DIAG_MAC_RESULT: {
        char rssi[24]="不可用";
        if(p[19]&RADIO_DIAG_MAC_ACK)snprintf(rssi,sizeof(rssi),"%d dBm",p[36]>=128?(int)p[36]-256:p[36]);
        log->event="radio_mac_result";
        if(p[18]){log->level="WARN";log->persist=1;}
        snprintf(out,capacity,"MAC发送最终结果 | 短地址=0x%04X | MAC句柄=%u | MAC选项=%s | 状态=0x%02X（%s）| MAC ACK=%s | 重试=%u | ACK RSSI=%s | txOptions=0x%04X | %s | MAC句柄不是AF事务号，按地址和时间辅助对照",
            log->address,p[34],!(p[19]&RADIO_DIAG_HAS_REQ)?"未知":(p[19]&RADIO_DIAG_INDIRECT)?"间接":"未设间接标志",
            p[18],radio_ti_status(p[18]),(p[19]&RADIO_DIAG_MAC_ACK)?"已确认":"未确认或未请求",
            p[35],rssi,
            radio_u16(p+32),polls);
        break;
    }
    case RADIO_DIAG_POLL:
        log->event="radio_poll_observed";
        snprintf(out,capacity,"收到MAC取包事件 | 短地址=0x%04X | noRsp=%u | %s | 仅证明协议栈上报了本次轮询，不代表某条AF回复已交付",
            log->address,p[37],polls);break;
    case RADIO_DIAG_COVERAGE:
        log->event="radio_diag_coverage";log->level="INFO";log->persist=1;
        snprintf(out,capacity,"被动无线观察已启用 | 观察钩子=%s | SDK appPendingQueue=%u（未修改）| 轮询上报覆盖不完整，无事件不能判定没有轮询；MAC结果不是发射开始事件",
            p[19]&2U?"已安装":"不可用",p[19]&1U?1U:0U);break;
    case RADIO_DIAG_DROPPED:
        log->event="radio_diag_dropped";log->level="WARN";log->persist=1;
        snprintf(out,capacity,"无线观察缓冲溢出 | 丢失诊断=%u | 原协议栈消息未拦截；本段观察不完整",radio_u32(p+24));break;
    }
    return 1;
}
#endif
