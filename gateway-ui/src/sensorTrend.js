export const TREND_LIMIT = 30;
export const TREND_METRICS = {
    temperature: { label: '温度', unit: '℃', digits: 2, series: [{ key: 'temperature', label: '温度', color: '#7c63e8' }] },
    digital: { label: '数字麦克风声级', unit: 'dB SPL', digits: 2, series: [{ key: 'digital_db', label: '数字麦克风', color: '#4191df' }] },
    ultrasonic: { label: '超声麦克风声级', unit: 'dB SPL', digits: 2, series: [{ key: 'analog_db', label: '超声麦克风', color: '#e99b35' }] },
    magnetic: { label: '磁场', unit: 'μT', digits: 1, series: [
        { key: 'mag_x', label: 'X 轴', color: '#5181ea' },
        { key: 'mag_y', label: 'Y 轴', color: '#12a982' },
        { key: 'mag_z', label: 'Z 轴', color: '#e68942' }
    ] }
};

export function normalizeTrendRecords(data, metric) {
    const config = TREND_METRICS[metric];
    if (!config || !Array.isArray(data)) return [];
    const batches = new Map();
    for (const item of data) {
        if (!item || item.batch_id == null || !Number.isFinite(Number(item.batch_id)) ||
            !Number.isFinite(Number(item.timestamp)) || Number(item.timestamp) <= 0) continue;
        if (config.series.some(({ key }) => item[key] == null || item[key] === '' ||
            !Number.isFinite(Number(item[key])))) continue;
        batches.set(String(item.batch_id), { ...item, timestamp: Number(item.timestamp),
            captured_at: Number.isFinite(Number(item.captured_at)) && Number(item.captured_at) > 0 ? Number(item.captured_at) : 0 });
    }
    return [...batches.values()].sort((a, b) => (a.captured_at || a.timestamp) - (b.captured_at || b.timestamp)).slice(-TREND_LIMIT);
}

export function trendBounds(records, series) {
    const values = records.flatMap(row => series.map(({ key }) => Number(row[key])));
    if (!values.length) return [0, 1];
    const low = Math.min(...values), high = Math.max(...values);
    const padding = Math.max((high-low)*0.15, high === low ? 0.5 : 0.1);
    return [low-padding, high+padding];
}
