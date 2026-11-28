import React from 'react';
import { Settings, Pencil, Trash2, Undo2, Check, X } from 'lucide-react';

export default function NodeActions({ renaming, saving, customName, onSettings, onRename, onDelete, onRestore, onSave, onCancel }) {
    const button = (label, Icon, tone, onClick, disabled = false) =>
        <button key={label} type="button" className={`node-icon-action ${tone}`} title={label} aria-label={label}
            onClick={onClick} disabled={disabled}><Icon size={16} strokeWidth={1.8} aria-hidden="true" /></button>;
    return <div className="node-icon-actions" role="group" aria-label="节点操作">
        {button('设置', Settings, 'action-settings', onSettings)}
        {renaming ? <>
            {button(saving ? '保存中…' : '保存名称', Check, 'action-save', onSave, saving)}
            {button('取消重命名', X, 'action-cancel', onCancel, saving)}
        </> : <>
            {button('重命名', Pencil, 'action-rename', onRename, saving)}
            {customName && button('恢复默认名称', Undo2, 'action-cancel', onRestore, saving)}
            {button('彻底删除', Trash2, 'action-delete', onDelete)}
        </>}
    </div>;
}
