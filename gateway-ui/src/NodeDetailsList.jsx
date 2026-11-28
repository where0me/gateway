import React, { useLayoutEffect, useRef, useState } from 'react';
import SensorTrendPanel from './SensorTrendPanel.jsx';
import { nodeCardWidth } from './nodeDetailsLayout.js';

export default function NodeDetailsList({ devices, selectedTrend, onCloseTrend, renderNode }) {
    const container = useRef(null);
    const [width, setWidth] = useState(null);
    useLayoutEffect(() => {
        const element = container.current;
        const measure = () => setWidth(nodeCardWidth(element.getBoundingClientRect().width));
        measure();
        const observer = typeof ResizeObserver !== 'undefined' ? new ResizeObserver(measure) : null;
        observer?.observe(element);
        window.addEventListener('resize', measure);
        return () => { observer?.disconnect(); window.removeEventListener('resize', measure); };
    }, []);
    return <div className="node-details-layout" ref={container}
        style={width === null ? undefined : { '--node-card-width': `${width}px` }}>
        {devices.map((dev, index) => <div className={`node-details-item${selectedTrend?.mac === dev.mac ? ' has-trend' : ''}`} key={dev.mac || index}>
            {renderNode(dev, index)}
            {selectedTrend?.mac === dev.mac && <SensorTrendPanel mac={dev.mac} name={dev.name || dev.mac}
                metric={selectedTrend.metric} onClose={onCloseTrend} />}
        </div>)}
    </div>;
}
