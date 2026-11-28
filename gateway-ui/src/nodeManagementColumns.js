export const NODE_COLUMNS = [
    ['name', '名称'], ['mac', 'MAC地址'], ['networkAddress', '网络地址'], ['lqi', 'LQI'], ['rssi', 'RSSI'], ['lastSeen', '最后出现'],
    ['availability', '可用性'], ['power', '电源'], ['battery', '剩余电量'], ['interview', '设备识别 / Cluster'], ['actions', '操作']
];
export const COLUMN_STORAGE_KEY = 'gateway.node-management.columns.v1';
export function readNodeColumns() {
    try {
        const saved = JSON.parse(localStorage.getItem(COLUMN_STORAGE_KEY));
        const result = Object.fromEntries(NODE_COLUMNS.map(([key]) => [key, typeof saved?.[key] === 'boolean' ? saved[key] : true]));
        if (Object.values(result).some(Boolean)) return result;
    } catch { /* Storage may be unavailable; use all columns. */ }
    return Object.fromEntries(NODE_COLUMNS.map(([key]) => [key, true]));
}
export function nodeLastSeen(node, observed) {
    // Do not use last_collection: collection time is not last communication time.
    const stored = Math.max(Number(node.link_updated_at) || 0, Number(node.last_active) || 0) * 1000;
    return Math.max(Number.isFinite(stored) && stored > 0 ? stored : 0, observed || 0);
}
export function relativeSeen(time, now) {
    if (!time) return '未上报';
    const seconds = Math.max(0, Math.floor((now - time) / 1000));
    if (seconds < 5) return '刚刚';
    if (seconds < 60) return `${seconds}秒前`;
    if (seconds < 3600) return `${Math.floor(seconds / 60)}分钟前`;
    if (seconds < 86400) return `${Math.floor(seconds / 3600)}小时前`;
    return `${Math.floor(seconds / 86400)}天前`;
}
export function batteryText(node) {
    if ([1, 2, 4, 5, 6].includes(node.power_source)) return 'N/A';
    if (node.power_source !== 3) return '未上报';
    const percent = node.battery_percentage;
    return typeof percent === 'number' && Number.isFinite(percent) && percent >= 0 && percent <= 100 ? `${percent}%` : '未上报';
}

export function networkAddress(value) {
    // Database text uses hexadecimal, including addresses consisting only of digits.
    const address = typeof value === 'string' && /^(?:0x)?[0-9a-f]{1,4}$/i.test(value)
        ? parseInt(value.replace(/^0x/i, ''), 16) : value;
    return Number.isInteger(address) && address >= 0 && address < 0xfff8
        ? { hex: `0x${address.toString(16).toUpperCase().padStart(4, '0')}`, decimal: address } : null;
}
