import React, { useState } from 'react';
import { createRoot } from 'react-dom/client';
import { AlertBar } from '../../../tests/fixtures/casecheetah/UI/src/shell/AlertBar';
import { NineSlice, panelArt } from '../../../tests/fixtures/casecheetah/UI/src/components/NineSlice';
import { useUIStore } from '../../../tests/fixtures/casecheetah/UI/src/stores/ui-store';
import type { AlertGroup } from '../../../tests/fixtures/casecheetah/UI/src/types';
import './profile.css';

// Local fixture host captures data; it never invokes an actual Unreal action.
const events: { name: string; data: any }[] = [];
window.ue = { sendEvent: (name, data) => events.push(JSON.parse(JSON.stringify({ name, data }))) };
const items = [
  { text: '伦敦集散仓 / London Depot', facility: 'warehouse-london', company: 'company-tide' },
  { text: '潮汐贸易公司 / Tide Trading', company: 'company-tide' },
];
function App() {
  const [many, setMany] = useState(false);
  const dossier = useUIStore(s => s.dossier);
  const dock = useUIStore(s => s.dock);
  const quotes = useUIStore(s => s.quotes);
  const last = events.at(-1)?.data;
  const alerts: AlertGroup[] = [{ kind: 'gate', name: '待办理的集散仓', items: many ? Array.from({length:13}, (_, i) => ({...items[i % 2], text: `${i + 1}. ${items[i % 2].text}`})) : items }];
  return <div id="gallery">
    <div id="caption">CaseCheetah / 中文提示与状态</div>
    <div id="subtitle">原样 AlertBar · Zustand · UE 数据桥</div>
    <NineSlice id="card" art={panelArt.card} style={{position:'absolute',left:28,top:76,width:584,height:298}}>
      <AlertBar alerts={alerts}/>
    </NineSlice>
    <div id="selection" data-events={JSON.stringify(events)} data-dossier={dossier !== null ? 'open' : 'closed'} data-dock={dock}>
      {last ? `已选择 ${last.kind === 'facility' ? '集散仓' : '公司'} / ${last.id}` : '点击「仓」展开，选择一项查看档案'}
    </div>
    <button id="many" aria-pressed={many} onClick={() => setMany(!many)}>切换 2 / 13 项</button>
    <button id="reply" onClick={() => window.__ue_dispatch?.('setQuote', {req:'fixture-q1', quote:{cost:42}, canOperate:true, suppliers:[]})}>接收 UE 示例回执</button>
    <div id="reply-status">{quotes['fixture-q1'] ? '回执已进入原始 store / fixture-q1' : '本地捕获桥事件，不执行游戏命令'}</div>
  </div>;
}
const errors: string[] = [];
const root = createRoot(document.getElementById('root')!, {onUncaughtError:e => errors.push(String(e))});
root.render(<App/>);
Object.assign(globalThis, { caseCheetahAlert: {
  version: React.version, errors, unmount: () => root.unmount(),
  snapshot: () => ({
    expanded: document.querySelector('.alert-chip')?.firstChild?.getAttribute('aria-expanded'),
    count: document.querySelector('.alert-chip')?.firstChild?.getAttribute('aria-label'),
    many: document.getElementById('many')?.getAttribute('aria-pressed'),
    selection: document.getElementById('selection')?.textContent,
    events: JSON.parse(document.getElementById('selection')?.getAttribute('data-events') ?? '[]'),
    dossier: document.getElementById('selection')?.getAttribute('data-dossier'),
    dock: document.getElementById('selection')?.getAttribute('data-dock'),
    reply: document.getElementById('reply-status')?.textContent,
  }),
} });
