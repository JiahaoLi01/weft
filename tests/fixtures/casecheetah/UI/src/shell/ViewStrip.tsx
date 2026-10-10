import { useEffect } from 'react';
import { pickView } from '@/lib/focus';
import { sendToUE } from '@/lib/ue-bridge';
import { measureLabel, setShareFocus } from '@/lib/format';
import { useUIStore } from '@/stores/ui-store';
import { Select } from '@/components/Select';
import { NineSlice, panelArt } from '@/components/NineSlice';
import { BuildButton } from '@/panels/BuildMenu';
import type { World } from '@/types';

// 视图条（HierarchyUI.md 第 4、5 节）：the map views of the focus's level, the first its default; then the base colouring and
// the world colourings, which belong to no level, the layers, and building.

const baseModes = [{ value: 'natural', label: '地貌' }, { value: 'country', label: '国家' }, { value: 'state', label: '行省' }, { value: 'province', label: '地块' }, { value: 'market', label: '集散区' }];
const worldModes = ['grain_price', 'harvest', 'build_difficulty'];
const metricViews = new Set(['share', 'units', 'revenue', 'stock', 'awareness', 'loyalty', 'delivered', 'main', 'price']);
// The markers: the focus's buildings (a company's, else the group's) and everyone's on the plot open on the left; or none.
const markerNames = { group: '焦点设施', off: '设施隐藏' } as const;

/** The good the goods map opens on: the last one looked at, else cloth, else the first. */
function lastGood(world: World): string {
  const goods = world.goods ?? [];
  let remembered = '';
  try { remembered = localStorage.getItem('bm.good') ?? ''; } catch { /* storage may be unavailable */ }
  if (world.shareFocus?.kind === 'good') return world.shareFocus.id;
  return goods.find(g => g.id === remembered)?.id ?? goods.find(g => g.id === 'cloth')?.id ?? goods[0]?.id ?? '';
}
const rememberGood = (id: string) => { try { localStorage.setItem('bm.good', id); } catch { /* per-viewer convenience only */ } };

export function ViewStrip({ world, connected }: { world: World; connected: boolean }) {
  const markerMode = useUIStore(s => s.markerMode);
  const dossierOpen = useUIStore(s => !!s.dossier || !!s.modal);
  const placeOpen = useUIStore(s => s.placeOpen && !s.modal && !s.settings);
  const cycleMarkers = useUIStore(s => s.cycleMarkers);
  const f = world.focus;
  const share = world.shareFocus;
  const goodsMode = world.mapMode === 'market_share' && share?.kind === 'good';
  // A good's colourings are its own level's views; the rest of the goods map is the old world colouring.
  const goodFocus = f?.kind === 'good';
  const worldMode = worldModes.includes(world.mapMode) || !!world.routeView || (goodsMode && !goodFocus) || world.mapMode === 'build_interest';
  const metric = world.mapMetric;
  const base = baseModes.some(m => m.value === world.mapMode) ? world.mapMode : '';
  const worldValue = world.routeView ? 'routes' : worldModes.includes(world.mapMode) ? world.mapMode : '';
  // 专题地图（HierarchyUI.md 第 8.2 节）：粮价、年景、修建难度、远程航线. A good's map is in the goods book.
  const worldOptions = [{ value: '', label: '专题地图' }, ...((world.mapModes ?? []).filter(m => worldModes.includes(m.id)).map(m => ({ value: m.id, label: m.name }))),
    { value: 'routes', label: '远程航线' }];
  // A base colouring under a metric view goes back to the level's first plain view.
  const plainView = f?.views.find(v => !metricViews.has(v.id))?.id;
  const pickBase = (mode: string) => {
    if (world.routeView) sendToUE('set_route_view', { enabled: false });
    sendToUE('set_map_mode', { mode });
    if (f && metricViews.has(f.view) && plainView) pickView(f.kind, plainView);
  };
  // A level view (or the 天下着色 placeholder) leaves the world colouring: the route view puts back the colouring before it.
  const pickLevel = (view: string) => {
    if (world.routeView) sendToUE('set_route_view', { enabled: false });
    if (f) pickView(f.kind, view);
  };
  const pickWorld = (value: string) => {
    if (value === 'goods') setShareFocus('good', lastGood(world), 'main');
    else if (value === 'routes') sendToUE('set_route_view', { enabled: true });
    else if (value) { if (world.routeView) sendToUE('set_route_view', { enabled: false }); sendToUE('set_map_mode', { mode: value }); }
    else if (world.routeView) sendToUE('set_route_view', { enabled: false });
    else if (worldMode && f) pickView(f.kind, f.view);
  };
  // 数字键 1–8：the focus's views, as numbered on the strip (not while typing).
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const target = e.target as HTMLElement | null;
      if (e.altKey || e.ctrlKey || e.metaKey || !f || (target && (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA' || target.isContentEditable))) return;
      const n = Number(e.key);
      if (!Number.isInteger(n) || n < 1 || n > 8 || n > f.views.length) return;
      pickLevel(f.views[n - 1].id);
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  });
  // For the in-engine smokes: a view, base or world colouring by its name, as picked on the strip.
  useEffect(() => {
    (window as unknown as { __bmMap: (name: string) => boolean }).__bmMap = (name: string) => {
      const view = f?.views.find(v => v.name === name);
      const baseMode = baseModes.find(m => m.label === name);
      const worldOption = worldOptions.find(o => o.label === name);
      if (view) pickLevel(view.id);
      else if (baseMode) pickBase(baseMode.value);
      else if (worldOption) pickWorld(worldOption.value);
      return !!(view || baseMode || worldOption);
    };
  });
  return <div className={`view-dock ${dossierOpen ? 'with-dossier' : ''} ${placeOpen ? 'with-place' : ''}`} role="group" aria-label="舆图视图">
    {metric?.numeric && <div className="metric-legend" aria-label={`${metric.name}色阶`}>
      <div className="metric-context">{metric.context}<span>{metric.scope} · {metric.hasValue ? `${metric.text}${metric.unit}` : '无数据'}</span></div>
      <div className="metric-scale"><span>{metric.min}{metric.unit}</span><i style={{ background: `linear-gradient(90deg, ${metric.low}, ${metric.high})` }} /><span>{metric.max}{metric.unit}</span><b style={{ backgroundColor: metric.missing }} /><span>无数据</span>
        {metric.hatch && <><b className="hatch-swatch" /><span>{metric.hatch}</span></>}</div>
    </div>}
    {goodsMode && share && <div className="share-focus">
      {!goodFocus && <Select value={share.id} onChange={g => { rememberGood(g); setShareFocus('good', g, share.measure ?? 'main'); }} ariaLabel="商品" minWidth={100}
        options={(world.goods ?? []).map(g => ({ value: g.id, label: g.name }))} />}
      <span className="focus-pick">选中商品<Select value={share.selected || 'none'} onChange={p => setShareFocus('good', share.id, p === 'none' && share.measure === 'delivered' ? 'main' : share.measure ?? 'main', p)} ariaLabel="选中商品" minWidth={130}
        options={[{ value: 'none', label: '不选' }, ...(share.products ?? []).map(p => ({ value: p.id, label: `${p.name}${p.company ? ` · ${p.company}` : ''}` }))]} /></span>
      {!goodFocus && <div className="seg small">{(share.measures ?? ['main']).map(m => <button key={m} className={(share.measure ?? 'main') === m ? 'active' : ''} aria-pressed={(share.measure ?? 'main') === m}
        onClick={() => setShareFocus('good', share.id, m, share.selected || 'none')}>{measureLabel('good', m)}</button>)}</div>}
    </div>}
    {world.routeView ? <div className="route-legend">
      <span><i className="rail" />铁路</span><span><i className="sea" />海运航线</span><span><i className="air" />空运（未开通）</span>
    </div> : <div className="map-key"><span>{world.hoverRoute || world.hoverSelection?.name || ''}</span>{world.cameraTier === 'near' && metric?.numeric && <span>近景不着色 · 拉远看着色</span>}
      {world.seaRoutes && <span><i />可通航 <i className="locked" />未启用</span>}{world.flows?.shown && (world.flows.good ? <span><i className="flow" />本集团 <i className="flow others" />别家 <i className="role-swatch make" />产 <i className="role-swatch use" />用</span>
        : world.flows.links && !world.flows.paths ? null : <span><i className="flow" />自运 <i className="flow carried" />经销{world.flows.bought ? <> <i className="flow bought" />进货</> : null}</span>)}
      {!!world.flows?.shown && !!world.flows.links && <span><i className="flow link" />集团内供需</span>}
      {f?.view === 'sources' && <span><i className="source-swatch" />到厂价 · 金色最便宜</span>}</div>}
    <NineSlice art={panelArt.card} className="view-strip">
      <div className="view-tabs map-buttons">{(f?.views ?? []).map((v, i) => {
        const active = !worldMode && f!.view === v.id;
        return <button key={v.id} disabled={!connected} className={active ? 'active' : ''} aria-pressed={active} title={`${i + 1}`} onClick={() => pickLevel(v.id)}>{v.name}</button>;
      })}</div>
      <div className="view-tools">
        <Select value={base || 'natural'} onChange={pickBase} ariaLabel="底图" minWidth={76} options={baseModes} />
        <Select value={worldValue} onChange={pickWorld} ariaLabel="专题地图" minWidth={96} options={worldOptions} />
        {!world.routeView && <button className={markerMode !== 'off' ? 'active' : ''} onClick={cycleMarkers}>{markerNames[markerMode]}</button>}
        {!world.routeView && <button disabled={!connected} className={world.seaRoutes ? 'active sea-switch' : 'sea-switch'} aria-pressed={world.seaRoutes} onClick={() => sendToUE('toggle_sea_routes')}>航路</button>}
        <BuildButton world={world} disabled={!connected} />
      </div>
    </NineSlice>
  </div>;
}
