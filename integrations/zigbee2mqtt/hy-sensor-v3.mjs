// Read-only discovery adapter. It deliberately does NOT send the HY HELLO:
// that would select a private collection host which this adapter doesn't implement.
import * as m from 'zigbee-herdsman-converters/lib/modernExtend';

const control = {
    name: 'hySensorControl', ID: 0xFC10,
    attributes: {
        protocolVersion: { ID: 0, type: 0x21 }, capabilities: { ID: 1, type: 0x19 },
        monitoringEnabled: { ID: 2, type: 0x10 }, intervalSeconds: { ID: 3, type: 0x23 },
        sensorMask: { ID: 4, type: 0x18 }, monitorState: { ID: 5, type: 0x30 },
        configValid: { ID: 6, type: 0x10 }, vibrationRateHz: { ID: 7, type: 0x21 },
    }, commands: {}, commandsResponse: {},
};
const data = {
    name: 'hySensorData', ID: 0xFC11,
    attributes: {
        batchId: { ID: 0, type: 0x23 }, validMask: { ID: 1, type: 0x18 },
        magneticX: { ID: 2, type: 0x29 }, magneticY: { ID: 3, type: 0x29 }, magneticZ: { ID: 4, type: 0x29 },
        digitalDb: { ID: 5, type: 0x20 }, analogDb: { ID: 6, type: 0x20 }, vibrationPoints: { ID: 7, type: 0x23 },
    }, commands: {}, commandsResponse: {},
};
export default {
    fingerprint: [{ modelID: 'HY-Sensor', manufacturerName: 'HuaYang', softwareBuildID: '3.0.0' }],
    model: 'HY-Sensor-v3', vendor: 'HuaYang',
    description: 'STM32WB55 BLE+Zigbee sensor: standard temperature and read-only HY capabilities',
    extend: [
        m.deviceAddCustomCluster('hySensorControl', control),
        m.deviceAddCustomCluster('hySensorData', data),
        m.temperature(),
        m.numeric({ name: 'protocol_version', cluster: 'hySensorControl', attribute: 'protocolVersion',
            description: 'HY private collection protocol version', access: 'STATE_GET', reporting: false }),
        m.numeric({ name: 'capabilities', cluster: 'hySensorControl', attribute: 'capabilities',
            description: 'Capability bitmap: temperature/magnetic/digital mic/analog mic/vibration', access: 'STATE_GET', reporting: false }),
    ],
};
