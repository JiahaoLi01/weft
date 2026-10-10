import { useEffect, useMemo, useState } from 'react';
import { Select } from '@/components/Select';
import { facilityGlyph, pounds } from '@/lib/format';
import { sendToUE } from '@/lib/ue-bridge';
import type { BuildMenuRow, World } from '@/types';

// 建筑（Resources.md 第 10 节、OperationsUI.md）：底部的“建筑”按钮列出可建的设施；选一种，舆图按它的兴趣着色，点地块打开
// 预填好的新建面板。

/** Enters the build view for a facility type and company, or leaves it (no type). */
export const setBuildMode = (type = '', company = '') => sendToUE('build_mode', { type, company });

/** One business: its facility types, and which of the group's companies builds them. */
function BusinessGroup({ name, rows, onPick }: { name: string; rows: BuildMenuRow[]; onPick: (row: BuildMenuRow, company: string) => void }) {
  const companies = rows[0]?.companies ?? [];
  const [company, setCompany] = useState(companies[0]?.id ?? '');
  useEffect(() => { if (!companies.some(c => c.id === company)) setCompany(companies[0]?.id ?? ''); }, [companies, company]);
  return <section className="build-group-row">
    <div className="build-business"><b>{name}</b>
      {companies.length === 1 && <em>{companies[0].name}</em>}
      {companies.length > 1 && <Select value={company} onChange={setCompany} ariaLabel={`${name}公司`} minWidth={150}
        options={companies.map(c => ({ value: c.id, label: `${c.name}（${pounds(c.cash)}）` }))} />}
    </div>
    <ul>{rows.map(r => <li key={r.type}>
      <button disabled={!companies.length} onClick={() => onPick(r, company)}>
        <i className="glyph">{facilityGlyph[r.type] ?? r.typeName.slice(0, 1)}</i><b>{r.typeName}</b>
        <span>{companies.length ? r.interest : r.reason}</span></button></li>)}</ul>
  </section>;
}

/** 建筑：the button on the bottom dock and its list of buildings. */
export function BuildButton({ world, disabled }: { world: World; disabled: boolean }) {
  const [open, setOpen] = useState(false);
  const menu = world.buildMenu ?? [];
  // Businesses the group runs first, then the rest (greyed: it needs a company of that business).
  const groups = useMemo(() => {
    const by = new Map<string, BuildMenuRow[]>();
    for (const r of menu) by.set(r.business, [...(by.get(r.business) ?? []), r]);
    return [...by.values()].sort((a, b) => Number(!a[0].companies.length) - Number(!b[0].companies.length));
  }, [menu]);
  useEffect(() => {
    if (!open) return;
    const close = (e: KeyboardEvent) => { if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); setOpen(false); } };
    window.addEventListener('keydown', close, true);
    return () => window.removeEventListener('keydown', close, true);
  }, [open]);
  const pick = (row: BuildMenuRow, company: string) => { setBuildMode(row.type, company); setOpen(false); };
  return <>
    <button className={`build-button${open || world.build ? ' active' : ''}`} disabled={disabled || !menu.length} aria-expanded={open} onClick={() => setOpen(!open)}>建筑</button>
    {open && <div className="build-picker" role="dialog" aria-label="可建的设施">
      <div className="build-picker-head"><b>建筑</b><span>选一种设施，舆图按它的兴趣着色；点地块打开新建面板。</span><button className="link" onClick={() => setOpen(false)}>关闭</button></div>
      <div className="build-picker-body">{groups.map(rows => <BusinessGroup key={rows[0].business} name={rows[0].businessName} rows={rows} onPick={pick} />)}</div>
    </div>}
  </>;
}

/** While building: what the colours mean, the hovered plot, and the way out. */
export function BuildBanner({ world }: { world: World }) {
  const b = world.build;
  useEffect(() => {
    if (!b) return;
    // Esc leaves the build view, unless a panel is open (its own Esc closes it first).
    const leave = (e: KeyboardEvent) => { if (e.key === 'Escape' && !document.querySelector('.modal-backdrop, [role="dialog"]')) setBuildMode(); };
    window.addEventListener('keydown', leave);
    return () => window.removeEventListener('keydown', leave);
  }, [b]);
  if (!b) return null;
  const note = world.mapMetric?.note;
  const near = world.cameraTier === 'near' || world.cameraTier === 'mid';
  return <div className="siting-banner build-banner" role="status">
    <b>建 {b.typeName}{b.companyName ? ` · ${b.companyName}` : ''}</b>
    <span>{b.interest}。{near ? '点击地块，打开新建面板。' : '放大到地块层级再点击。'}</span>
    <button className="link" onClick={() => setBuildMode()}>退出（Esc）</button>
    {note && <em>{note}</em>}
  </div>;
}
