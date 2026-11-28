export const GROUP_COLUMNS = [
    ['name', '组名'], ['id', 'ID'], ['description', '描述'], ['members', '成员数量'],
    ['online', '在线数量'], ['sampling', '采样参数'], ['actions', '操作']
];
export const GROUP_COLUMNS_KEY = 'gateway.groups.columns.v1';
export function readGroupColumns(storage) {
    try {
        const source = storage || (typeof localStorage !== 'undefined' ? localStorage : null);
        const saved = JSON.parse(source?.getItem(GROUP_COLUMNS_KEY) || 'null');
        const columns = Object.fromEntries(GROUP_COLUMNS.map(([key]) => [key, typeof saved?.[key] === 'boolean' ? saved[key] : true]));
        if (Object.values(columns).some(Boolean)) return columns;
    } catch { /* Fall back to all columns if storage is unavailable or invalid. */ }
    return Object.fromEntries(GROUP_COLUMNS.map(([key]) => [key, true]));
}
