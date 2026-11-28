const labels = { interviewing: '正在识别', ready: '采集协议就绪', standard: '标准设备（无采集协议）', failed: '识别未完成' };
export function interviewLabel(interview) { return labels[interview?.state] || '尚未识别'; }
export function interviewSummary(interview) {
    if (!interview) return '等待协调器读取设备描述符';
    const hex = value => `0x${Number(value).toString(16).toUpperCase().padStart(4, '0')}`;
    const lines = [];
    if (interview.protocol != null) lines.push(`采集协议版本：${interview.protocol}`);
    if (interview.capabilities != null) lines.push(`能力位图：${hex(interview.capabilities)}`);
    for (const [ep, descriptor] of Object.entries(interview.endpoints || {})) {
        lines.push(`端点 ${ep} / Profile ${hex(descriptor.profile)}`,
            `服务端：${(descriptor.input || []).map(hex).join(', ') || '无'}`,
            `客户端：${(descriptor.output || []).map(hex).join(', ') || '无'}`);
    }
    return lines.join('\n') || '等待协调器读取设备描述符；识别失败不会删除节点';
}
