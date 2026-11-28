import assert from 'node:assert/strict';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { createServer } from 'vite';
const server = await createServer({ server: { middlewareMode: true }, appType: 'custom' });
try {
    const { default: Sidebar } = await server.ssrLoadModule('/src/GatewaySidebar.jsx');
    const { SIDEBAR_STORAGE_KEY, readSidebarCollapsed, NAVIGATION_ITEMS } = await server.ssrLoadModule('/src/gatewayNavigation.js');
    assert.equal(readSidebarCollapsed(), false);
    globalThis.localStorage = { getItem: key => { assert.equal(key, SIDEBAR_STORAGE_KEY); return 'true'; } };
    assert.equal(readSidebarCollapsed(), true);
    const render = activeTab => renderToStaticMarkup(React.createElement(Sidebar, { activeTab, onSelect() {} }));
    const collapsed = render('groups');
    assert.match(collapsed, /gateway-sidebar is-collapsed/);
    assert.match(collapsed, /aria-label="展开导航"/);
    assert.match(collapsed, /aria-expanded="false"/);
    assert.equal((collapsed.match(/<button /g) || []).length, 9);
    for (const [, label] of NAVIGATION_ITEMS) assert.ok(collapsed.includes(`aria-label="${label}"`));
    globalThis.localStorage = { getItem: () => { throw new Error('blocked'); } };
    assert.equal(readSidebarCollapsed(), false);
    const expanded = render('node-settings');
    assert.match(expanded, /aria-label="折叠导航"/);
    assert.match(expanded, /aria-expanded="true"/);
    assert.equal((expanded.match(/aria-current="page"/g) || []).length, 1);
    assert.match(expanded, /title="节点管理" aria-current="page"/);
    assert.equal((expanded.match(/<svg /g) || []).length, 9);
    delete globalThis.localStorage;
    console.log('Sidebar: eight routes, icon labels, active node settings, collapse persistence, storage fallback PASS');
} finally { await server.close(); }
