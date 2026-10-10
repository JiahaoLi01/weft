import React, { useState } from 'react';
import { focusOn } from '@/lib/focus';
import { sendToUE } from '@/lib/ue-bridge';
import { emblemArt } from '@/lib/emblems';
import { useUIStore } from '@/stores/ui-store';
import { count, decimal, facilityGlyph, percent, pounds, price, shortPounds } from '@/lib/format';
import type { BusinessTree as Tree, TreeKind, FocusView, TreeCompany, TreeFacility, TreeLine, TreeProduct, World } from '@/types';

// 经营树（HierarchyUI.md 第 3 节）：the player's group, or the group observed, as a ledger: companies, their brands and products,
// their buildings and lines. Picking a row makes it the focus (the map follows); the focused row opens into a card with its
// key figures and the way into its dossier.

const goodArt = (id: string) => `art/goods/${id || 'unknown'}.png`;

/** 字徽：a neutral round ink monogram in the company's colour (HierarchyUI.md 第 9 节). */
export function Seal({ text, hue, kind = 'company' }: { text: string; hue?: number; kind?: 'group' | 'company' | 'brand' }) {
  return <i className={`seal ${kind}`} style={hue !== undefined ? { ['--seal-hue' as string]: String(hue) } as React.CSSProperties : undefined}>{text}</i>;
}

export function GoodArt({ id }: { id: string }) {
  return <img className="good-art" src={goodArt(id)} alt="" draggable={false} />;
}

/** A building's emblem: its painted object, or its type character in an ink disc while that type has none. */
export function FacilityMark({ typeId, type }: { typeId: string; type: string }) {
  const art = emblemArt(typeId);
  return art ? <img className="facility-emblem" src={art} alt="" draggable={false} /> : <i className="facility-mark">{facilityGlyph[typeId] ?? type.slice(0, 1)}</i>;
}

const statusTone = (s?: string) => !s ? '' : /封存|没有产品/.test(s) ? 'idle' : /受限|缺|欠/.test(s) ? 'limited' : /工程|安装|未开工/.test(s) ? 'building' : 'running';
const unknown = '未知';

function Row({ kind, id, depth, icon, name, note, figure, tone, dot, open, hasChildren, onToggle, focus, card, preview, children }: {
  kind: TreeKind; id: string; depth: number; icon: React.ReactNode; name: string; note?: string; figure?: React.ReactNode; tone?: string; dot?: string;
  open: boolean; hasChildren: boolean; onToggle: () => void; focus?: FocusView; card?: React.ReactNode; preview?: string[]; children?: React.ReactNode;
}) {
  const openDossier = useUIStore(s => s.openDossier);
  const setPreview = useUIStore(s => s.setPreview);
  const selected = focus?.kind === kind && focus.id === id;
  // 预览（HierarchyUI.md 第 7 节）：the row's buildings light up on the map while the pointer is on it.
  return <li className={`tree-node depth-${depth} ${selected ? 'selected' : ''}`} data-node={`${kind}:${id}`}>
    <div className="tree-row" role="button" tabIndex={0} aria-current={selected} onClick={() => focusOn(kind, id)}
      onMouseEnter={() => setPreview(preview ?? [])} onMouseLeave={() => setPreview([])}
      onDoubleClick={() => { focusOn(kind, id); openDossier(); }} onKeyDown={e => { if (e.key === 'Enter') focusOn(kind, id); }}>
      <span className={`twist ${hasChildren ? (open ? 'open' : 'closed') : 'leaf'}`} onClick={e => { if (hasChildren) { e.stopPropagation(); onToggle(); } }} />
      {icon}
      <span className="tree-name">{name}{note && <em>{note}</em>}</span>
      {dot && <i className={`ink-dot ${dot}`} title={dot} />}
      {figure !== undefined && <span className={`tree-figure ${tone ?? ''}`}>{figure}</span>}
    </div>
    {selected && card}
    {open && hasChildren && <ul className="tree-children">{children}</ul>}
  </li>;
}

/** The focused row's card: three or four figures and the ways onward. */
function FocusCard({ figures, actions, intel }: { figures: [string, React.ReactNode][]; actions?: React.ReactNode; intel?: number }) {
  const openDossier = useUIStore(s => s.openDossier);
  return <div className="focus-card">
    <dl>{figures.map(([label, value]) => <div key={label}><dt>{label}</dt><dd>{value}</dd></div>)}</dl>
    <div className="focus-actions">
      <button className="brush-button" onClick={() => openDossier()}>档案</button>
      {actions}
      {intel !== undefined && intel < 100 && <span className="intel-note">情报 {intel}</span>}
    </div>
  </div>;
}

const money = (v?: number) => v === undefined ? unknown : shortPounds(v);
const share = (v?: number) => v === undefined ? unknown : percent(v);

function ProductRow({ p, depth, focus, intel, preview }: { p: TreeProduct; depth: number; focus?: FocusView; intel: number; preview?: string[] }) {
  return <Row kind="product" id={p.productId} depth={depth} icon={<GoodArt id={p.goodId} />} name={p.product} note={p.good} preview={preview}
    figure={`${price(p.price)}`} open={false} hasChildren={false} onToggle={() => {}} focus={focus}
    card={<FocusCard intel={intel} figures={[['标价', `${price(p.price)}／${p.unit}`], ['上周销量', p.units === undefined ? unknown : `${count(p.units)} ${p.unit}`],
      ['上周收入', money(p.revenue)], ['份额', share(p.share)]]} />} />;
}

function LineRow({ l, depth, focus, intel }: { l: TreeLine; depth: number; focus?: FocusView; intel: number }) {
  const amount = (v?: number) => v === undefined ? unknown : `${decimal(v)} ${l.unit}`;
  return <Row kind="line" id={l.key} depth={depth} icon={<GoodArt id={l.goodId} />} name={`${l.id} 号线`} note={l.good} preview={[l.key.split('#')[0]]}
    figure={l.nominal ? percent((l.produced ?? 0) / l.nominal) : undefined} dot={statusTone(l.status)} open={false} hasChildren={false} onToggle={() => {}} focus={focus}
    card={<FocusCard intel={intel} figures={[['昨日产出', amount(l.produced)], ['额定', amount(l.nominal)], ['状态', l.status ?? unknown]]} />} />;
}

function FacilityRow({ f, depth, focus, intel, isOpen, toggle }: { f: TreeFacility; depth: number; focus?: FocusView; intel: number; isOpen: (k: string) => boolean; toggle: (k: string) => void }) {
  const key = `facility:${f.id}`;
  return <Row kind="facility" id={f.id} depth={depth} icon={<FacilityMark typeId={f.typeId} type={f.type} />} name={f.name} note={f.type} preview={[f.id]}
    figure={f.ratio !== undefined && f.ratio >= 0 ? <span className="util"><i style={{ width: `${Math.min(100, f.ratio * 100)}%` }} /></span> : undefined}
    dot={statusTone(f.status)} open={isOpen(key)} hasChildren={f.lines.length > 0} onToggle={() => toggle(key)} focus={focus}
    card={<FocusCard intel={intel} figures={[['状态', f.status ?? unknown], ['上周利用', f.ratio === undefined ? unknown : f.ratio < 0 ? '—' : percent(f.ratio)],
      ['产线', f.lines.length ? `${f.lines.length} 条` : '—'], ['规模', f.size === undefined ? unknown : String(f.size)]]} />}>
    {f.lines.map(l => <LineRow key={l.key} l={l} depth={depth + 1} focus={focus} intel={intel} />)}
  </Row>;
}

function CompanyRow({ c, focus, isOpen, toggle }: { c: TreeCompany; focus?: FocusView; isOpen: (k: string) => boolean; toggle: (k: string) => void }) {
  const key = `company:${c.id}`;
  const products = c.brands.reduce((n, b) => n + b.products.length, 0) + c.products.length;
  // A product's buildings: those with a line of its good (the tree carries no allocation).
  const makers = (goods: string[]) => c.facilities.filter(f => f.lines.some(l => goods.includes(l.goodId))).map(f => f.id);
  return <Row kind="company" id={c.id} depth={1} icon={<Seal text={c.emblem} hue={c.hue} />} name={c.name} note={c.business} preview={c.facilities.map(f => f.id)}
    figure={c.cash === undefined ? undefined : shortPounds(c.cash)} tone={c.lastOperating !== undefined && c.lastOperating < 0 ? 'negative' : ''}
    open={isOpen(key)} hasChildren={products + c.facilities.length > 0} onToggle={() => toggle(key)} focus={focus}
    card={<FocusCard intel={c.intel} figures={[['现金', money(c.cash)], ['上周经营利润', c.lastOperating === undefined ? unknown : pounds(c.lastOperating)],
      ['建筑', `${c.facilities.length} 处`], ['产品', `${products} 种`]]} />}>
    {c.brands.map(b => {
      const bk = `brand:${b.id}`;
      return <Row key={b.id} kind="brand" id={b.id} depth={2} icon={<Seal text={b.emblem} hue={c.hue} kind="brand" />} name={b.name} note="品牌" preview={makers(b.products.map(p => p.goodId))}
        open={isOpen(bk)} hasChildren={b.products.length > 0} onToggle={() => toggle(bk)} focus={focus}
        card={<FocusCard intel={c.intel} figures={[['产品', `${b.products.length} 种`], ['上周收入', b.products.some(p => p.revenue === undefined) ? unknown : shortPounds(b.products.reduce((a, p) => a + (p.revenue ?? 0), 0))]]} />}>
        {b.products.map(p => <ProductRow key={p.productId} p={p} depth={3} focus={focus} intel={c.intel} preview={makers([p.goodId])} />)}
      </Row>;
    })}
    {c.products.map(p => <ProductRow key={p.productId} p={p} depth={2} focus={focus} intel={c.intel} preview={makers([p.goodId])} />)}
    {!!c.facilities.length && <li className="tree-section">建筑</li>}
    {c.facilities.map(f => <FacilityRow key={f.id} f={f} depth={2} focus={focus} intel={c.intel} isOpen={isOpen} toggle={toggle} />)}
  </Row>;
}

export function BusinessTree({ world }: { world: World }) {
  const tree: Tree | undefined = world.tree;
  const focus = world.focus;
  // Rows the player opened or closed by hand; the rest are open when they lead to the focus.
  const [manual, setManual] = useState<Record<string, boolean>>({});
  if (!tree || !tree.id) return <p className="quiet tree-empty">{world.playerGroup ? '正在读取经营树…' : '开局后，这里是你的集团、公司与建筑。'}</p>;
  const onPath = new Set((focus?.path ?? []).map(p => `${p.kind}:${p.id}`));
  const isOpen = (k: string) => manual[k] ?? onPath.has(k);
  const toggle = (k: string) => setManual(m => ({ ...m, [k]: !isOpen(k) }));
  return <div className="business-tree">
    {focus?.observed && <div className="observe-banner">
      <span>观察：<b>{tree.name}</b></span><em>情报 {focus.intel}</em>
      {world.playerGroup && <button className="link" onClick={() => focusOn('group', world.playerGroup!)}>回到自己的集团</button>}
    </div>}
    <nav className="breadcrumbs" aria-label="焦点路径">
      <span className="history" role="group" aria-label="焦点历史">
        <button disabled={!focus?.canBack} aria-label="回到上一个焦点（Alt+←）" title="Alt+←" onClick={() => sendToUE('navigate', { dir: 'back' })}>‹</button>
        <button disabled={!focus?.canForward} aria-label="前进（Alt+→）" title="Alt+→" onClick={() => sendToUE('navigate', { dir: 'forward' })}>›</button>
      </span>
      {(focus?.path ?? []).map((p, i, all) =>
      <span key={`${p.kind}:${p.id}`}><button className={i === all.length - 1 ? 'current' : ''} onClick={() => focusOn(p.kind, p.id)}>{p.name}</button>{i < all.length - 1 && <i>›</i>}</span>)}</nav>
    <ul className="ledger-tree">
      <Row kind="group" id={tree.id} depth={0} icon={<Seal text={tree.name.slice(0, 1)} kind="group" />} name={tree.name} preview={tree.companies.flatMap(c => c.facilities.map(f => f.id))}
        figure={tree.familyCash === undefined ? undefined : shortPounds(tree.familyCash)} open={true} hasChildren={tree.companies.length > 0} onToggle={() => {}} focus={focus}
        card={<FocusCard intel={tree.mine ? 100 : focus?.intel} figures={[['家族资金', money(tree.familyCash)], ['旗下公司', `${tree.companies.length} 家`]]} />}>
        {tree.companies.map(c => <CompanyRow key={c.id} c={c} focus={focus} isOpen={isOpen} toggle={toggle} />)}
      </Row>
    </ul>
  </div>;
}
