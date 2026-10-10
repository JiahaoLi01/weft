import { useState } from 'react';
import { useUIStore } from '@/stores/ui-store';
import type { AlertGroup } from '@/types';

// 提示栏：一排字标，每类一个、带数字；点开列出每一项，点一项打开那一页（那座集散仓，或那家公司）。

const glyphs: Record<AlertGroup['kind'], string> = { gate: '仓' };

export function AlertBar({ alerts }: { alerts: AlertGroup[] }) {
  const [open, setOpen] = useState('');
  if (!alerts.length) return null;
  const go = (x: AlertGroup['items'][number]) => {
    const ui = useUIStore.getState();
    if (x.facility) ui.openModal({ kind: 'facility', id: x.facility });
    else ui.openModal({ kind: 'company', id: x.company });
    setOpen('');
  };
  return <div className="alert-bar" role="status" aria-label="提示">{alerts.map(a => <div key={a.kind} className={`alert-chip ${a.kind}`}>
    <button aria-expanded={open === a.kind} aria-label={`${a.name} ${a.items.length} 项`} onClick={() => setOpen(open === a.kind ? '' : a.kind)}>
      <i>{glyphs[a.kind] ?? '!'}</i><b>{a.items.length}</b><span>{a.name}</span></button>
    {open === a.kind && <ul className="alert-list">
      {a.items.slice(0, 12).map((x, i) => <li key={i}><button className="link" onClick={() => go(x)}>{x.text}</button></li>)}
      {a.items.length > 12 && <li className="quiet">另有 {a.items.length - 12} 项</li>}
    </ul>}
  </div>)}</div>;
}
