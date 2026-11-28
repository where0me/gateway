import { House, ChartNoAxesCombined, Server, RadioTower, ScrollText, FolderOpen, Network, Settings } from 'lucide-react';
export const SIDEBAR_STORAGE_KEY = 'gateway.sidebar.collapsed.v1';
export const NAVIGATION_ITEMS = [
    ['home', '首页', House], ['overview', '节点状态', ChartNoAxesCombined],
    ['gateway-status', '网关状态', Server], ['coordinator-status', '协调器状态', RadioTower],
    ['logs', '日志', ScrollText], ['groups', '分组', FolderOpen],
    ['devices', '节点管理', Network], ['settings', '设置', Settings]
];
export function readSidebarCollapsed() {
    try { return localStorage.getItem(SIDEBAR_STORAGE_KEY) === 'true'; }
    catch { return false; }
}
