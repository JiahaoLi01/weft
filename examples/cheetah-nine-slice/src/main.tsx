import React, { useState } from 'react';
import { createRoot } from 'react-dom/client';
import { NineSlice, panelArt } from '../../../tests/fixtures/casecheetah/UI/src/components/NineSlice';
import './profile.css';

// Authored fixture shell; the imported game component and art stay byte-exact.
// No game stores, Unreal bridge or labels are replaced inside that component.
function App() {
  const [wide, setWide] = useState(false);
  return <div id="gallery">
    <div className="caption">CaseCheetah / painted surfaces</div>
    <div className="subtitle">Original NineSlice.tsx and three original PNGs</div>
    <NineSlice id="dock-left" art={panelArt.dockLeft} style={{ position: 'absolute', left: 0, top: 0, width: 190, height: 420 }}>
      <div className="dock-label">Left archive</div>
    </NineSlice>
    <NineSlice id="dock-right" art={panelArt.dockRight} style={{ position: 'absolute', left: 476, top: 80, width: 164, height: 340 }}>
      <div className="dock-right-label">Right book</div>
    </NineSlice>
    <NineSlice id="card" art={panelArt.card} style={{ position: 'absolute', left: 196, top: 88, width: wide ? 360 : 270, height: wide ? 244 : 232 }}>
      <div className="panel-label">Floating card</div><div className="marker" />
    </NineSlice>
    <button id="resize" aria-label="Resize painted card" aria-pressed={wide} onClick={() => setWide(!wide)}>Resize card</button>
    <div id="status" className="status">{wide ? 'Wide: 360 x 244 / fixed brush edge widths' : 'Small: 270 x 232 / fixed brush edge widths'}</div>
  </div>;
}
const errors: string[] = [];
const root = createRoot(document.getElementById('root')!, { onUncaughtError: e => errors.push(String(e)) });
root.render(<App />);
Object.assign(globalThis, { caseCheetahFixture: {
  version: React.version, errors,
  unmount: () => root.unmount(),
  snapshot: () => ({ wide: document.getElementById('resize')?.getAttribute('aria-pressed'), text: document.getElementById('status')?.textContent }),
} });
