import React, { useState } from 'react';
import { CloudUpload, RadioTower } from 'lucide-react';
import MqttSettings from './MqttSettings.jsx';
import './SettingsPage.css';

export default function SettingsPage({ children }) {
    const [tab, setTab] = useState('mqtt');
    return <div className="gateway-settings">
        <div className="settings-tabs" role="tablist" aria-label="设置分类">
            {[['mqtt', 'MQTT 上传', CloudUpload], ['zigbee', 'Zigbee 网络', RadioTower]].map(([key, label, Icon]) =>
                <button key={key} type="button" id={`settings-tab-${key}`} role="tab" aria-selected={tab === key}
                    aria-controls={`settings-panel-${key}`} tabIndex={tab === key ? 0 : -1}
                    className={tab === key ? 'is-active' : ''} onClick={() => setTab(key)}
                    onKeyDown={event => {
                        if (['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) {
                            event.preventDefault();
                            const next = event.key === 'Home' ? 'mqtt' : event.key === 'End' ? 'zigbee' : key === 'mqtt' ? 'zigbee' : 'mqtt';
                            setTab(next);
                            document.getElementById(`settings-tab-${next}`)?.focus();
                        }
                    }}><Icon size={17} strokeWidth={1.7} />{label}</button>)}
        </div>
        <div id="settings-panel-mqtt" role="tabpanel" aria-labelledby="settings-tab-mqtt" hidden={tab !== 'mqtt'}><MqttSettings /></div>
        <div id="settings-panel-zigbee" role="tabpanel" aria-labelledby="settings-tab-zigbee" hidden={tab !== 'zigbee'}>{children}</div>
    </div>;
}
