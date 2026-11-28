#ifndef UPLOAD_TRACE_FORMAT_H
#define UPLOAD_TRACE_FORMAT_H
#include "upload_trace.h"
#include "radio_diag.h"
#include <stdio.h>
static inline const char *ut_stage(unsigned stage){
    switch(stage){
    case UT_COORD_RX:return "协调器应用层收到";
    case UT_COORD_FORWARD:return "协调器串口转发结果";
    case UT_COORD_DROP:return "协调器拒绝上行";
    case UT_HOST_RX:return "后端串口收到";
    case UT_HOST_REPLY:return "后端生成回复";
    case UT_COORD_SUBMIT:return "协调器提交回复";
    case UT_COORD_RESULT:return "协调器回复投递结果";
    case UT_NODE_RX:return "节点应用层收到回复";
    case UT_NODE_TX:return "节点提交上行";
    case UT_NODE_RESULT:return "节点上行发送结果";
    case UT_HOST_WRITE:return "后端串口写入结果";
    default:return "诊断边界";
    }
}
static inline const char *ut_command(unsigned cmd){
    switch(cmd){case 0x92:return "批次申请";case 0x93:return "窗口描述";case 0x94:return "波形分片";
    case 0x95:return "上传查询";case 0x0D:return "上传回复";case 0xA5:return "传感器清单";
    case 0x86:return "温度";case 0x87:return "磁场";case 0x88:return "数字麦克风";
    case 0x89:return "超声麦克风";case 0x8C:return "振动特征";case 0x8D:return "监测状态";default:return "其他";}
}
static inline int ut_format(const uint8_t *p,size_t n,char *out,size_t capacity){
    if(!ut_valid(p,n))return 0;
    if(p[1]==UT_CAPABILITY){
        snprintf(out,capacity,"协调器支持上传链路诊断 v3 | 环形记录=%u条 | 单次最多回放=%u条 | 同节点回放间隔=%u ms | 常态不逐片打印",ut_u32(p+24),ut_u16(p+28),ut_u32(p+36));return 1;
    }
    if(p[1]==UT_SNAPSHOT || p[1]==UT_SNAPSHOT_END){
        snprintf(out,capacity,"%s | 短地址=0x%04X | 原因编号=%u | 快照=%u | 环形覆盖总数=%u | 仅为最近有限记录，未记录不代表未收到",
            p[1]==UT_SNAPSHOT?"协调器收发快照开始":"协调器收发快照结束",ut_u16(p+6),p[11],ut_u32(p+24),ut_u32(p+36));return 1;
    }
    char block[16]="-",fragment[16]="-",seq[16]="未提供",extra[220]="";
    if(ut_u16(p+16)!=UINT16_MAX)snprintf(block,sizeof(block),"%u",(unsigned)ut_u16(p+16)+1U);
    if(p[19]&UT_FRAGMENT_VALID)snprintf(fragment,sizeof(fragment),"%u",(unsigned)p[18]+1U);
    if(p[19]&UT_SEQ_VALID)snprintf(seq,sizeof(seq),"%u",p[9]);
    if(p[1]==UT_COORD_SUBMIT || p[1]==UT_COORD_RESULT)
        snprintf(extra,sizeof(extra)," | AF事务=%u | 投递状态=%s（0x%02X）| 耗时=%u ms",p[30],radio_ti_status(p[31]),p[31],ut_u32(p+36));
    else if(p[1]==UT_COORD_FORWARD || p[1]==UT_HOST_WRITE)
        snprintf(extra,sizeof(extra)," | 串口=%s（仅本机写入结果，不代表对端接收）",p[11]?"未完整写入":"完整写入");
    else if(p[1]==UT_COORD_DROP)
        snprintf(extra,sizeof(extra)," | 拒绝原因=%s",p[11]==1?"子节点表没有此地址":p[11]==2?"采集协议尚未识别完成":p[11]==4?"传感器清单格式无效":"端点、集群或帧方向不匹配");
    else if(p[1]==UT_COORD_RX)
        snprintf(extra,sizeof(extra)," | RSSI=%d dBm | LQI=%u",p[30]>=128?(int)p[30]-256:p[30],p[31]);
    snprintf(out,capacity,"%s | 短地址=0x%04X | 批次=%u | 窗口=%s | 分片=%s | 消息=%s（0x%02X）| ZCL序号=%s | 内容指纹=%08X | 类型=%s（编号%u）| 结果字段=0x%02X | 附加值=%08X | 长度=%u | 源记录=%u | 源时间=%u ms%s%s",
        ut_stage(p[1]),ut_u16(p+6),ut_u32(p+12),block,fragment,ut_command(p[8]),p[8],seq,
        ut_u32(p+20),p[8]==0x0D?radio_ack_kind(p[10]):"-",p[10],p[11],ut_u32(p+24),ut_u16(p+28),ut_u32(p+32),ut_u32(p+2),
        extra,p[19]&UT_SNAPSHOT_FLAG?" | 异常回放":"");
    return 1;
}
#endif
