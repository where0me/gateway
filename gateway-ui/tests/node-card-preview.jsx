import React from 'react';
import { createRoot } from 'react-dom/client';
import Card from '../src/NodeDetails';
import '../src/App.css';

// Isolated layout fixtures: never inserted into the live application/database.
const nodes = [
    { name: 'HY-Sensor-0E54', status: 'Online', lqi: 149, rssi: -46, power_source: 4 },
    { name: 'HY-Sensor-9D2C', status: 'Online', lqi: 167, rssi: -40, power_source: 4,
        temp: '24.50', digital_db: '70.00', analog_db: '69.00', mag: true,
        vibration_features: [[.01,.039,.076,2.966,7819.7],[.01,.037,.073,2.974,8049.7],[.012,.048,.091,2.964,6947.7]],
        raw_upload_status: 'complete', last_sample_time: '2026/10/12 20:30:00' },
    { name: '长名称节点-设备编号-00000000000000000000', status: 'Offline' }
];
createRoot(document.getElementById('root')).render(<main style={{ padding: 16, maxWidth: 900 }}>
    <div className="compact-node-grid">{nodes.map((dev, index) => <Card key={index} dev={dev} index={index}
        parseMagData={mag => mag ? { x: '56.0', y: '-98.7', z: '-58.2' } : null} onRemove={() => {}} />)}</div>
</main>);
