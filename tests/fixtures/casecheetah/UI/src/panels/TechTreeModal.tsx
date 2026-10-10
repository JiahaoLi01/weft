import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Modal, Tabs, Term } from '@/components/Modal';
import { Select } from '@/components/Select';
import { getZoom } from '@/components/tooltip/TooltipSystem';
import { command, percent, pounds } from '@/lib/format';
import { isInUnreal } from '@/lib/ue-bridge';
import { TECH_ICONS } from '@/lib/techIcons';
import { useUIStore } from '@/stores/ui-store';
import type { TechEdge, TechNode, World } from '@/types';

// 科技树（TechTree.md 第 7 节）：one trade a page; its components are lanes, the sciences a band on top, the years run from the
// opening (1844) to 1972 and what came before stands in the opening column. Lines: within a component (solid, with the distance),
// an explicit link to another component or trade (dashed), a science to what needs it (broken while nobody has the science).

const START = 1844, END = 1972;
const LABEL_W = 118, OPEN_W = 324, YEAR_W = 9, NODE_W = 150, NODE_H = 46, ROW_H = 56, GAP = 8, SHIFT = 44, AXIS_H = 30, LANE_PAD = 8;
const MIN_ZOOM = .35, MAX_ZOOM = 2;
const ULTRALIGHT = isInUnreal();
const xOfYear = (year: number) => LABEL_W + OPEN_W + (Math.min(END, Math.max(START, year)) - START) * YEAR_W;
const WIDTH = xOfYear(END) + NODE_W / 2 + 16;

type Spot = { x: number; y: number };
type Lane = { id: string; name: string; kind: 'science' | 'method' | 'design' | 'ghost'; y: number; height: number };
type Ghost = { id: string; name: string; industry: string; note: string };

/** Places a lane's nodes: those before the opening in a grid in the opening column, the rest at their year, packed into rows
 *  (a node moves right a little to share a row, else takes the next). */
function placeLane(items: { id: string; year: number; at?: number }[], top: number, spots: Map<string, Spot>) {
  const before = items.filter(i => i.year < START && i.at === undefined).sort((a, b) => a.year - b.year || a.id.localeCompare(b.id));
  const after = items.filter(i => i.year >= START || i.at !== undefined)
    .sort((a, b) => (a.at ?? xOfYear(a.year)) - (b.at ?? xOfYear(b.year)) || a.id.localeCompare(b.id));
  let rows = Math.ceil(before.length / 2);
  before.forEach((item, i) => spots.set(item.id, { x: LABEL_W + GAP + (i % 2) * (NODE_W + GAP), y: top + LANE_PAD + Math.floor(i / 2) * ROW_H }));
  const right: number[] = [];
  for (const item of after) {
    const want = item.at ?? xOfYear(item.year) - NODE_W / 2;
    let row = right.findIndex(r => want >= r + GAP || r + GAP - want <= SHIFT);
    if (row < 0) { row = right.length; right.push(-Infinity); }
    const x = Math.max(want, right[row] + GAP);
    right[row] = x + NODE_W;
    spots.set(item.id, { x, y: top + LANE_PAD + row * ROW_H });
  }
  rows = Math.max(rows, right.length, 1);
  return rows * ROW_H + LANE_PAD * 2 - (ROW_H - NODE_H);
}

/** 超前：years still to a tech's first year, to a tenth under ten. */
const aheadYears = (days: number) => { const y = days / 365; return y < 10 ? y.toFixed(1).replace(/\.0$/, '') : String(Math.round(y)); };
/** A weekly chance, readable down to a few in ten thousand. */
const chanceText = (p: number) => p >= .001 ? percent(p) : `${(p * 100).toFixed(p >= .0001 ? 2 : 3)}%`;

/** One line in a word: what the company has of the tech. Its first year is no wall (超前): the line says how far ahead it is. */
function status(t: TechNode) {
  if (t.class === 'science') return t.known ? '已掌握' : '本轮还没有获取途径';
  if (t.inUse) return '在用';
  if (t.known) return t.public ? '公共知识' : t.disposal === 'patent' ? '已掌握 · 专利' : t.source === 'licence' ? '已授权' : '已掌握';
  if (t.researching) return `在研 ${percent(t.progress)}${t.stalled ? ' · 停付' : ''}`;
  if (t.missing.length) return `缺：${t.missing.join('、')}`;
  const ahead = t.daysAhead > 0 ? `超前 ${aheadYears(t.daysAhead)} 年` : '';
  if (t.discovered) return t.progress > 0 ? `已发现 · 进度 ${percent(t.progress)}` : ahead ? `已发现 · ${ahead}` : '已发现';
  if (t.pressure > 0) return ahead ? `灵感 ${percent(t.pressure)} · ${ahead}` : `灵感 ${percent(t.pressure)}`;
  return ahead || '未发现';
}

function tone(t: TechNode) {
  if (t.known) return t.inUse ? 'known in-use' : 'known';
  if (t.class === 'science' || t.missing.length) return 'gated';
  if (t.discovered) return 'discovered';
  return t.daysAhead > 0 ? 'future' : 'unknown';
}

/** A curve between two nodes: sideways when one stands clear to the left, otherwise up or down. */
function curve(a: Spot, b: Spot) {
  if (b.x >= a.x + NODE_W) {
    const x1 = a.x + NODE_W, y1 = a.y + NODE_H / 2, x2 = b.x, y2 = b.y + NODE_H / 2, k = Math.max(24, (x2 - x1) / 2);
    return { d: `M${x1},${y1} C${x1 + k},${y1} ${x2 - k},${y2} ${x2},${y2}`, mx: (x1 + x2) / 2, my: (y1 + y2) / 2 };
  }
  if (a.x >= b.x + NODE_W) return curve(b, a);
  const down = b.y > a.y;
  const x1 = a.x + NODE_W / 2, y1 = down ? a.y + NODE_H : a.y, x2 = b.x + NODE_W / 2, y2 = down ? b.y : b.y + NODE_H, k = Math.max(16, Math.abs(y2 - y1) / 2);
  return { d: `M${x1},${y1} C${x1},${y1 + (down ? k : -k)} ${x2},${y2 - (down ? k : -k)} ${x2},${y2}`, mx: (x1 + x2) / 2, my: (y1 + y2) / 2 };
}

function Detail({ t, tree, onClose }: { t: TechNode; tree: { company: string; operate: boolean; year: number }; onClose: () => void }) {
  const patentable = t.class === 'product' || t.class === 'machine';
  const research = tree.operate && !t.known && !t.researching && t.quote?.ok;
  return <aside className="tech-detail">
    <small>{t.className} · {t.categoryName}</small>
    <h2>{t.name}</h2>
    <p className="tech-years">{t.year && t.year >= START ? `史实 ${t.year} 年` : '1844 年以前就有'}{t.availableFrom > 0 ? ` · ${t.availableFrom} 年以前算超前` : ''}</p>
    {t.note && <p className="quiet">{t.note}</p>}
    <dl>
      {t.effects && <><dt>效果</dt><dd>{t.effects}</dd></>}
      {t.equipment && <><dt>设备／级</dt><dd>{t.equipment}</dd></>}
      <dt>本公司</dt><dd>{status(t)}</dd>
      {!t.known && t.class !== 'science' && t.pressure > 0 && <><dt><Term name="灵感压力">在产产线按产量、按距离给它加压，封顶 25%；没有超前时它就是每周发现的概率。</Term></dt><dd>{percent(t.pressure)}</dd></>}
      {!t.discovered && t.class !== 'science' && !t.missing.length && (t.pressure > 0 || t.daysAhead > 0) && <>
        <dt><Term name="发现率">每周按它掷一次骰：灵感压力 × 超前的折扣。史实年份不是硬门槛：每超前一年发现率减半，产线越大、旧技术用得越多，越早发现；研发也每超前一年多花五成的钱和时间。</Term></dt>
        <dd>{t.chance > 0 ? `每周 ${chanceText(t.chance)}` : '还没有灵感'}{t.daysAhead > 0 ? `（超前 ${aheadYears(t.daysAhead)} 年）` : ''}</dd></>}
      {t.requires.length > 0 && <><dt>基础学科</dt><dd>{t.missing.length ? <span className="tech-gap">缺 {t.missing.join('、')}：断开，本轮够不着</span> : '已具备'}</dd></>}
      {t.links.length > 0 && <><dt><Term name="连接">写明的技术距离，可以跨部件类、跨行业：灵感沿它走，研发报价也按它算最近距离。</Term></dt>
        <dd>{t.links.map(l => `${l.name} ${l.distance.toFixed(2)}`).join('、')}</dd></>}
      {t.quote && <><dt>研发报价</dt><dd>{t.quote.ok ? `${pounds(t.quote.cost)} · ${t.quote.weeks} 周` : t.quote.reason}
        {!t.quote.ok && t.quote.cost > 0 && <span className="detail-note">发现以后：{pounds(t.quote.cost)} · {t.quote.weeks} 周</span>}
        {t.quote.cost > 0 && (t.quote.nearest || t.quote.ahead > 1.001) && <span className="detail-note">
          {[t.quote.nearest ? `离最近已掌握的 ${t.quote.nearest} ${t.quote.distance.toFixed(2)}` : '', t.quote.premium ? '对手有专利 ×1.3' : '',
            t.quote.ahead > 1.001 ? `超前 ×${t.quote.ahead.toFixed(2)}` : ''].filter(Boolean).join(' · ')}</span>}</dd></>}
      {t.patent && <><dt>专利</dt><dd>{t.patent.holder} · {t.patent.expires} 到期
        {t.patent.claimedFrom && <span className="detail-note">从 {t.patent.claimedFrom} 上收{t.patent.relatedParty ? ' · 关联交易' : ''}</span>}</dd></>}
    </dl>
    {tree.operate && <div className="tech-actions">
      {research && (patentable
        ? <><button className="brush-button" onClick={() => command('knowledge.research', tree.company, { tech: t.id, disposal: 'patent' })}>研发 · 申请专利</button>
          <button className="link" onClick={() => command('knowledge.research', tree.company, { tech: t.id, disposal: 'hidden' })}>研发 · 保密</button></>
        : <button className="brush-button" onClick={() => command('knowledge.research', tree.company, { tech: t.id })}>研发</button>)}
      {t.canPatent && <button className="link" onClick={() => command('knowledge.dispose', tree.company, { tech: t.id, choice: 'patent' })}>申请专利</button>}
      {t.canPublish && <button className="link" onClick={() => command('knowledge.dispose', tree.company, { tech: t.id, choice: 'public' })}>公开</button>}
      {t.patent?.claimants.map(c => <button key={c.id} className="link" onClick={() => command('knowledge.claim_patent', c.id, { tech: t.id })}>
        <Term name={`上收到 ${c.name}`}>集团对专利持有人有 75% 以上的参与投票股份时，上面的信托或控股公司可以无条件拿走专利；持有人照样能用。不是全资时记一桩关联交易。</Term></button>)}
      {t.licensor && t.licensees?.map(l => <button key={l.id} className="link" onClick={() => command('knowledge.offer_licence', t.licensor!, { tech: t.id, licensee: l.id, fee: '0', royalty: '0' })}>
        {t.licensor === tree.company ? '' : `${t.licensorName} `}授权给 {l.name}</button>)}
    </div>}
    <button className="link tech-close" onClick={onClose}>收起</button>
  </aside>;
}

/** 拖拽平移、滚轮缩放：the canvas moves under a fixed window; the wheel and the buttons zoom about the cursor (or the middle). */
function usePanZoom() {
  const port = useRef<HTMLDivElement | null>(null);
  // The window as state too: the wheel listener goes onto whichever element is mounted (the loading one, then the tree's).
  const [node, setNode] = useState<HTMLDivElement | null>(null);
  const ref = useCallback((el: HTMLDivElement | null) => { port.current = el; setNode(el); }, []);
  const [view, setView] = useState({ x: 0, y: 0, zoom: 1 });
  const [dragging, setDragging] = useState(false);
  // Whether the last press moved the canvas: its click then picks nothing.
  const moved = useRef(false);
  /** A pointer in the window's own layout pixels (Ultralight: rects are logical, clientX is physical). */
  const at = (e: { clientX: number; clientY: number }) => {
    const el = port.current;
    if (!el) return { x: 0, y: 0 };
    const z = getZoom(), r = el.getBoundingClientRect();
    return ULTRALIGHT ? { x: e.clientX / z - r.left, y: e.clientY / z - r.top } : { x: (e.clientX - r.left) / z, y: (e.clientY - r.top) / z };
  };
  const zoomBy = (factor: number, about?: { x: number; y: number }) => setView(v => {
    const zoom = Math.min(MAX_ZOOM, Math.max(MIN_ZOOM, v.zoom * factor));
    const p = about ?? { x: (port.current?.clientWidth ?? 0) / 2, y: (port.current?.clientHeight ?? 0) / 2 };
    const k = zoom / v.zoom;
    return { zoom, x: p.x - (p.x - v.x) * k, y: p.y - (p.y - v.y) * k };
  });
  const fit = (width: number, height: number) => {
    const el = port.current;
    if (!el || !width || !height) return;
    const zoom = Math.min(1.2, Math.max(MIN_ZOOM, Math.min(el.clientWidth / width, el.clientHeight / height)));
    setView({ zoom, x: Math.max(0, (el.clientWidth - width * zoom) / 2), y: 0 });
  };
  const zoomRef = useRef(zoomBy);
  zoomRef.current = zoomBy;
  useEffect(() => {
    if (!node) return;
    // Not passive: the wheel zooms the tree instead of scrolling anything under it.
    const onWheel = (e: WheelEvent) => { e.preventDefault(); zoomRef.current(e.deltaY < 0 ? 1.12 : 1 / 1.12, at(e)); };
    node.addEventListener('wheel', onWheel, { passive: false });
    return () => node.removeEventListener('wheel', onWheel);
  }, [node]);
  const onMouseDown = (e: React.MouseEvent) => {
    if (e.button !== 0 && e.button !== 1) return;
    const z = getZoom(), start = { cx: e.clientX, cy: e.clientY, x: view.x, y: view.y };
    moved.current = false;
    const move = (ev: MouseEvent) => {
      const dx = (ev.clientX - start.cx) / z, dy = (ev.clientY - start.cy) / z;
      if (!moved.current && Math.abs(dx) + Math.abs(dy) < 4) return;
      if (!moved.current) setDragging(true);
      moved.current = true;
      setView(v => ({ ...v, x: start.x + dx, y: start.y + dy }));
    };
    const up = () => { setDragging(false); window.removeEventListener('mousemove', move); window.removeEventListener('mouseup', up); };
    window.addEventListener('mousemove', move);
    window.addEventListener('mouseup', up);
  };
  return { ref, port, view, setView, dragging, moved, zoomBy, fit, onMouseDown };
}

export function TechTreeModal({ world, company, industry, onClose }: { world: World; company: string; industry?: string; onClose: () => void }) {
  const openModal = useUIStore(s => s.openModal);
  const tree = world.techTree?.company === company ? world.techTree : undefined;
  const [page, setPage] = useState(industry ?? '');
  const [selected, setSelected] = useState('');
  const pan = usePanZoom();
  // + and - zoom (not while typing in a form).
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.target instanceof HTMLInputElement) return;
      if (e.key === '+' || e.key === '=') pan.zoomBy(1.2);
      else if (e.key === '-') pan.zoomBy(1 / 1.2);
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  });
  const current = tree?.industries.find(i => i.id === page) ?? tree?.industries[0];
  const layout = useMemo(() => {
    const spots = new Map<string, Spot>();
    const lanes: Lane[] = [];
    if (!tree || !current) return { spots, lanes, height: 0, ghosts: [] as Ghost[], techs: [] as TechNode[] };
    const techs = tree.techs.filter(t => t.industry === current.id || t.class === 'science');
    const here = new Set(techs.filter(t => t.class !== 'science').map(t => t.id));
    // The sciences this trade needs or links to, then the trade's components, then the other trades' techs linked to it.
    const sciences = techs.filter(t => t.class === 'science' && tree.edges.some(e => e.from === t.id && here.has(e.to)));
    let y = AXIS_H;
    const add = (lane: Omit<Lane, 'y' | 'height'>, items: { id: string; year: number; at?: number }[]) => {
      if (!items.length) return;
      const height = placeLane(items, y, spots);
      lanes.push({ ...lane, y, height });
      y += height;
    };
    add({ id: 'science', name: '基础学科', kind: 'science' }, sciences);
    for (const c of current.categories)
      add({ id: c.id, name: c.name, kind: c.class === '生产方式' ? 'method' : 'design' }, techs.filter(t => t.category === c.id && t.class !== 'science'));
    // 别的行业：another trade's tech linked to one here stands below, under the one it is near.
    const ghosts: Ghost[] = [];
    const ghostItems: { id: string; year: number; at?: number }[] = [];
    const isScience = (id: string) => sciences.some(s => s.id === id);
    for (const e of tree.edges) {
      if (e.kind !== 'link') continue;
      const inward = here.has(e.to) && !here.has(e.from) && !isScience(e.from);
      const outward = here.has(e.from) && !here.has(e.to) && !isScience(e.to);
      if (!inward && !outward) continue;
      const [inside, outside, name, trade] = inward ? [e.to, e.from, e.fromName, e.fromIndustry] : [e.from, e.to, e.toName, e.toIndustry];
      if (ghosts.some(g => g.id === outside)) continue;
      const target = spots.get(inside);
      const near = techs.find(t => t.id === inside);
      ghosts.push({ id: outside, name: name ?? outside, industry: trade ?? '', note: `${trade ?? ''} · 离${near?.name ?? ''} ${e.distance.toFixed(2)}` });
      ghostItems.push({ id: outside, year: START, at: target ? Math.max(LABEL_W + GAP, target.x) : undefined });
    }
    add({ id: 'ghost', name: '别的行业', kind: 'ghost' }, ghostItems);
    return { spots, lanes, height: y + 12, ghosts, techs };
  }, [tree, current]);
  if (!tree) return <Modal eyebrow="科技树" title="正在读取…" layer="fullscreen" className="tech-tree-modal" onClose={onClose}>
    <div ref={pan.ref} className="tech-tree-viewport"><p className="quiet">正在读取科技树…</p></div></Modal>;
  const byId = new Map(layout.techs.map(t => [t.id, t]));
  const chosen = byId.get(selected);
  const edges = tree.edges.filter(e => layout.spots.has(e.from) && layout.spots.has(e.to));
  // A distance is written where the line has room (a long sideways run), and on the chosen node's lines.
  const label = (e: TechEdge, a: Spot, b: Spot) => {
    if (e.kind === 'gate' && e.distance >= 1) return '';
    const lit = !!selected && (e.from === selected || e.to === selected);
    return lit || Math.abs(b.x - a.x) - NODE_W >= 80 ? e.distance.toFixed(2) : '';
  };
  const pick = (id: string) => { if (!pan.moved.current) setSelected(selected === id ? '' : id); };
  return <Modal className="tech-tree-modal" layer="fullscreen" onClose={onClose}
    eyebrow={<>科技树 · {tree.companyName}</>}
    title={current?.name ?? '科技树'}
    meta={<>
      <span>{tree.year} 年</span>
      {tree.companies.length > 1 && <span className="tech-company">看
        <Select value={company} ariaLabel="看哪家公司" minWidth={120} options={tree.companies.map(c => ({ value: c.id, label: c.name }))}
          onChange={id => openModal({ kind: 'tech-tree', company: id, industry: current?.id })} /></span>}
      <span><Term name="技术距离">同一部件类里按技能要求的差别算，写明的连接可以跨部件类、跨行业，两边取小。距离 0.9 以内的在产技术给未发现的技术攒灵感；研发报价按离最近已掌握技术的距离加价。有的技术要基础学科：没有它就够不着，这个行业的技术到此为止。</Term></span>
      <span className="tech-legend"><i className="known" />已掌握 <i className="discovered" />已发现 <i className="unknown" />未发现 <i className="gated" />断开</span></>}
    tabs={<Tabs tabs={tree.industries.map(i => ({ id: i.id, name: i.name }))} value={current?.id ?? ''}
      onChange={id => { setPage(id); setSelected(''); pan.setView(v => ({ ...v, x: 0, y: 0 })); }} />}>
    <div className={`tech-tree-body ${chosen ? 'with-detail' : ''}`}>
      <div ref={pan.ref} className={`tech-tree-viewport ${pan.dragging ? 'dragging' : ''}`} onMouseDown={pan.onMouseDown}>
        <div className="tech-zoom" onMouseDown={e => e.stopPropagation()}>
          <button className="link" aria-label="缩小" onClick={() => pan.zoomBy(1 / 1.2)}>−</button>
          <span>{Math.round(pan.view.zoom * 100)}%</span>
          <button className="link" aria-label="放大" onClick={() => pan.zoomBy(1.2)}>+</button>
          <button className="link" onClick={() => pan.fit(WIDTH, layout.height)}>适合窗口</button>
          <button className="link" onClick={() => pan.setView({ x: 0, y: 0, zoom: 1 })}>原大</button>
          <em>拖拽平移 · 滚轮缩放</em>
        </div>
        {/* Moved by a translate, scaled by zoom: under transform: scale() above about 1.5 Ultralight sets the text at the wrong
            advances (the glyphs pile into each other); zoom lays it out again at the size it shows, as the page's UI scale does. */}
        <div className="tech-tree-pan" style={{ transform: `translate(${Math.round(pan.view.x)}px, ${Math.round(pan.view.y)}px)` }}>
        <div className="tech-tree-canvas" style={{ width: WIDTH, height: layout.height, zoom: pan.view.zoom }}>
          <div className="tech-axis">
            <span className="tech-open" style={{ left: LABEL_W, width: OPEN_W }}>1844 年以前</span>
            {Array.from({ length: (END - 1850) / 10 + 1 }, (_, i) => 1850 + i * 10).map(y =>
              <span key={y} className="tech-tick" style={{ left: xOfYear(y) }}>{y}</span>)}
          </div>
          <i className="tech-now" style={{ left: xOfYear(tree.year), height: layout.height - AXIS_H }} />
          {layout.lanes.map(l => <div key={l.id} className={`tech-lane ${l.kind}`} style={{ top: l.y, height: l.height }}><span>{l.name}</span></div>)}
          <svg className="tech-edges" width={WIDTH} height={layout.height}>
            {edges.map((e, i) => {
              const a = layout.spots.get(e.from)!, b = layout.spots.get(e.to)!;
              const c = curve(a, b);
              const target = byId.get(e.to);
              const open = e.kind === 'gate' && !!target?.missing.length;
              return <g key={i} className={`tech-edge ${e.kind} ${open ? 'open' : ''} ${selected && (e.from === selected || e.to === selected) ? 'lit' : ''}`}>
                <path d={c.d} />
                {open ? <text x={c.mx} y={c.my + 4} className="tech-break">断</text>
                  : label(e, a, b) && <text x={c.mx} y={c.my - 4}>{label(e, a, b)}</text>}
              </g>;
            })}
          </svg>
          {layout.techs.filter(t => layout.spots.has(t.id)).map(t => {
            const s = layout.spots.get(t.id)!;
            const bar = t.known ? 0 : t.researching || t.progress > 0 ? t.progress : t.pressure;
            // A tech without its own painting yet shows the placeholder (a rolled drawing), dimmed.
            const icon = TECH_ICONS.has(t.id);
            return <button key={t.id} className={`tech-node ${tone(t)} ${t.class === 'science' ? 'science' : ''} ${selected === t.id ? 'selected' : ''} with-icon`}
              style={{ left: s.x, top: s.y, width: NODE_W, height: NODE_H }} onClick={() => pick(t.id)}>
              <img className={`tech-icon ${icon ? '' : 'placeholder'}`} src={`art/techs/${icon ? t.id : 'placeholder'}.png`} alt="" draggable={false} />
              <b>{t.name}</b><em>{status(t)}</em>
              {bar > 0 && <i className={`tech-bar ${t.researching || t.progress > 0 ? 'progress' : 'pressure'}`} style={{ width: `${Math.min(100, bar * 100)}%` }} />}
              {t.patent && <i className="tech-seal" aria-label={`专利 · ${t.patent.holder}`}>专</i>}
            </button>;
          })}
          {layout.ghosts.map(g => {
            const s = layout.spots.get(g.id);
            return s && <span key={g.id} className="tech-node ghost" style={{ left: s.x, top: s.y, width: NODE_W, height: NODE_H }}><b>{g.name}</b><em>{g.note}</em></span>;
          })}
        </div>
        </div>
      </div>
      {chosen && <Detail t={chosen} tree={tree} onClose={() => setSelected('')} />}
    </div>
  </Modal>;
}
