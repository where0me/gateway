import React, { useState } from 'react';
import { PanelLeftClose, PanelLeftOpen } from 'lucide-react';
import { SIDEBAR_STORAGE_KEY, NAVIGATION_ITEMS, readSidebarCollapsed } from './gatewayNavigation';
import './GatewaySidebar.css';

export default function GatewaySidebar({ activeTab, onSelect }) {
    const [collapsed, setCollapsed] = useState(readSidebarCollapsed);
    const toggleLabel = collapsed ? '展开导航' : '折叠导航';
    const ToggleIcon = collapsed ? PanelLeftOpen : PanelLeftClose;
    function toggle() {
        const next = !collapsed;
        setCollapsed(next);
        try { localStorage.setItem(SIDEBAR_STORAGE_KEY, String(next)); }
        catch { /* Navigation still works when browser storage is blocked. */ }
    }
    return <aside className={`gateway-sidebar${collapsed ? ' is-collapsed' : ''}`} aria-label="网关侧边栏">
        <header className="gateway-sidebar-header">
            <button type="button" className="gateway-sidebar-toggle" onClick={toggle}
                aria-label={toggleLabel} title={toggleLabel} aria-expanded={!collapsed} aria-controls="gateway-primary-nav">
                <ToggleIcon size={19} strokeWidth={1.8} aria-hidden="true" />
            </button>
            <div className="gateway-sidebar-brand"><strong>Zigbee</strong><span>工业网关</span></div>
        </header>
        <nav id="gateway-primary-nav" className="gateway-sidebar-nav" aria-label="主导航">
            {NAVIGATION_ITEMS.map(([tab, label, Icon]) => {
                const selected = activeTab === tab || (tab === 'devices' && activeTab === 'node-settings');
                return <button key={tab} type="button" className={`gateway-nav-item${selected ? ' is-active' : ''}`}
                    onClick={() => onSelect(tab)} aria-label={label} title={label} aria-current={selected ? 'page' : undefined}>
                    <Icon className="gateway-nav-icon" size={19} strokeWidth={1.7} aria-hidden="true" />
                    <span className="gateway-nav-label">{label}</span>
                </button>;
            })}
        </nav>
    </aside>;
}
