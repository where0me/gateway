import assert from 'node:assert/strict';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { createServer } from 'vite';
const server = await createServer({ server: { middlewareMode: true }, appType: 'custom' });
try {
    const { default: Actions } = await server.ssrLoadModule('/src/NodeActions.jsx');
    const render = props => renderToStaticMarkup(React.createElement(Actions, props));
    const html = render({});
    for (const label of ['设置', '重命名', '彻底删除']) {
        assert.ok(html.includes(`title="${label}"`));
        assert.ok(html.includes(`aria-label="${label}"`));
    }
    assert.equal((html.match(/<button /g) || []).length, 3);
    assert.match(render({ customName: 'Custom' }), /恢复默认名称/);
    const busy = render({ renaming: true, saving: true });
    assert.match(busy, /保存中/);
    assert.equal((busy.match(/disabled/g) || []).length, 2);
    console.log('Icon actions: labels, normal/rename/saving/default-name states PASS');
} finally { await server.close(); }
