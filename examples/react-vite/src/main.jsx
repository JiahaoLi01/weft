import React, { useState } from 'react';
import { createRoot } from 'react-dom/client';

const font = { fontFamily: 'ProbeRoboto', fontWeight: 400, fontSize: 16, lineHeight: '24px', color: '#e7edf6', whiteSpace: 'nowrap' };
const flex = { display: 'flex', boxSizing: 'border-box', flexShrink: 0, textAlign: 'left', ...font };
const column = { ...flex, flexDirection: 'column' };
const text = (size = 16, color = '#e7edf6') => ({ ...column, fontSize: size, lineHeight: `${size + 8}px`, color });
const initial = [
  { id: 'ember', name: 'Ember', amount: 72, color: '#f4a261' },
  { id: 'tide', name: 'Tide', amount: 48, color: '#61c6d4' },
  { id: 'moss', name: 'Moss', amount: 60, color: '#92c879' },
];

function App() {
  const [count, setCount] = useState(0);
  const [items, setItems] = useState(initial);
  return <div id="page" style={{ ...column, width: 640, height: 420, padding: 24, gap: 16, backgroundColor: '#10151e' }}>
    <div id="header" style={{ ...column, height: 72, gap: 4 }}>
      <div id="eyebrow" style={text(12, '#61c6d4')}>CASE ELEPHANT / NATIVE UI</div>
      <div id="title" style={text(28)}>React + Vite</div>
    </div>
    <div id="content" style={{ ...flex, height: 192, gap: 16 }}>
      <div id="counter-card" style={{ ...column, width: 200, height: 192, padding: 16, gap: 8, backgroundColor: '#202b3d' }}>
        <div id="counter-label" style={text(14, '#aab9ce')}>React state</div>
        <div id="count" style={text(32)}>Count: {count}</div>
        <button id="increment" onClick={() => setCount(value => value + 1)} style={{ ...column, width: 168, height: 44, padding: 8, borderWidth: 0, backgroundColor: '#326cc2', ...text(16) }}>Add one</button>
      </div>
      <div id="list-card" style={{ ...column, flexGrow: 1, minWidth: 0, height: 192, padding: 16, gap: 8, backgroundColor: '#182331' }}>
        <div id="list-label" style={text(14, '#aab9ce')}>Keyed components</div>
        <div id="list" style={{ ...column, gap: 8 }}>
          {items.map(item => <div key={item.id} id={`item-${item.id}`} data-key={item.id} style={{ ...flex, height: 28, gap: 12 }}>
            <div id={`name-${item.id}`} style={{ ...text(14), width: 52 }}>{item.name}</div>
            <div id={`bar-${item.id}`} style={{ ...column, width: item.amount * 2, height: 16, marginTop: 4, backgroundColor: item.color }} />
          </div>)}
        </div>
      </div>
    </div>
    <div id="footer" style={{ ...flex, height: 44, gap: 16 }}>
      <button id="reverse" onClick={() => setItems(value => [...value].reverse())} style={{ ...column, width: 168, height: 44, padding: 8, borderWidth: 0, backgroundColor: '#28394d' }}>Reverse list</button>
      <div id="status" style={{ ...text(14, '#aab9ce'), paddingTop: 8 }}>DOM / Flex / FreeType / GPU</div>
    </div>
    <div id="note" style={text(12, '#7e91ab')}>One React app. Browser and native pixels.</div>
  </div>;
}

const errors = [];
const root = createRoot(document.getElementById('root'), {
  onUncaughtError: error => errors.push(String(error)),
  onCaughtError: error => errors.push(String(error)),
  onRecoverableError: error => errors.push(String(error)),
});
root.render(<App />);
globalThis.caseElephantReact = {
  version: React.version,
  errors,
  unmount: () => root.unmount(),
  snapshot: () => ({
    version: React.version,
    count: document.getElementById('count')?.textContent,
    order: Array.from(document.getElementById('list')?.childNodes ?? []).map(node => node.getAttribute('data-key')),
    errors: [...errors],
  }),
};
