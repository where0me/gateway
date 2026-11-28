// Keep a local switch intent until a device-list request started after its
// save completed can confirm the current server state. Older polls are stale.
export function createMonitoringSwitchState() {
    let version = 0;
    const edits = new Map();
    const value = device => {
        const edit = edits.get(device.mac);
        return edit && !edit.confirmed ? edit.enabled : Boolean(device.online_monitoring_enabled);
    };
    return {
        version: () => version,
        value,
        begin(device, enabled) {
            if (edits.get(device.mac)?.pending) return null;
            const edit = { mac: device.mac, enabled: Boolean(enabled), previous: value(device), pending: true, version: ++version };
            edits.set(device.mac, edit);
            return edit;
        },
        saved(edit, enabled) {
            if (edits.get(edit.mac) !== edit) return;
            edit.enabled = Boolean(enabled);
            edit.pending = false;
            edit.version = ++version;
        },
        failed(edit) {
            if (edits.get(edit.mac) === edit) {
                edits.delete(edit.mac);
                version++;
            }
        },
        snapshot(device, requestVersion) {
            const edit = edits.get(device.mac);
            if (!edit) return device;
            if (edit.pending || requestVersion < edit.version)
                return { ...device, online_monitoring_enabled: edit.enabled };
            // Keep the version watermark: an even older request may still
            // arrive after this confirmation from another refresh path.
            edit.confirmed = true;
            edit.enabled = Boolean(device.online_monitoring_enabled);
            return device;
        }
    };
}
