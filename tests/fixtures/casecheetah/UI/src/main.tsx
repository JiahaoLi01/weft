import { useEffect, useRef, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { TooltipProvider } from './components/tooltip/TooltipSystem';
import { onUECall, sendToUE, isInUnreal } from './lib/ue-bridge';
import { command, count, facilityGlyph, openCompany, people, percent, pounds, shortPounds } from './lib/format';
import { focusOn } from './lib/focus';
import { Modal, Term } from './components/Modal';
import { UtilBars } from './components/charts';
import { NineSlice, panelArt } from './components/NineSlice';
import { useUIStore } from './stores/ui-store';
import { FloatingUIOverlay } from './overlays/FloatingUIOverlay';
import { LineBriefs } from './panels/FacilityModal';
import { QuoteText } from './panels/Step4';
import { Deposits } from './panels/Mining';
import { Farmers } from './panels/Farming';
import { MarketPreview } from './panels/MarketEconomy';
import { BuildBanner } from './panels/BuildMenu';
import { BusinessTree } from './shell/BusinessTree';
import { ViewStrip } from './shell/ViewStrip';
import { DossierHost, WorkbenchHost } from './shell/Dossier';
import { PlaceDock } from './shell/PlaceDock';
import { WorldTree } from './shell/WorldTree';
import { GoodsBook } from './shell/GoodsBook';
import { CountryArchive } from './shell/CountryArchive';
import { AlertBar } from './shell/AlertBar';
import type { BuildOption, Facility, TreeKind, World } from './types';
import './style.css';
import './operations.css';
import './design/painted-anchors.css';
import './shell/shell.css';

// UI scale choices; 0 follows the window: the layout is drawn for 1600×1000 and grows in quarter steps.
const uiScales = [{ value: 0, name: '自动' }, { value: 1, name: '100%' }, { value: 1.25, name: '125%' }, { value: 1.5, name: '150%' }, { value: 1.75, name: '175%' }, { value: 2, name: '200%' }];
const autoScale = () => Math.max(1, Math.min(2.5, Math.floor(Math.min(window.innerWidth / 1600, window.innerHeight / 1000) * 4) / 4));
// What a click selects at the current camera tier and layer, as named in the archive and the map key.
const domainNoun = { province: '地块', market: '集散区', country: '国家' } as const;

/** A works in an archive list: status, last week's use of capacity, and the way into its panel. */
function FacilityList({ facilities, empty }: { facilities: Facility[]; empty: string }) {
  const openModal = useUIStore(s => s.openModal);
  if (!facilities.length) return <p className="quiet">{empty}</p>;
  return <ul className="facility-list">{facilities.map(f => {
    const line = f.production;
    const rated = line?.goods.filter(g => g.nominal > 0) ?? [];
    const ratio = rated.length ? rated.reduce((a, g) => a + g.produced / g.nominal, 0) / rated.length : undefined;
    return <li key={f.id}>
      <span className="facility-title"><i className="glyph">{facilityGlyph[f.typeId ?? ''] ?? f.type.slice(0, 1)}</i><b>{f.name}</b></span>
      <span>{f.type}{f.size > 1 ? ` · 规模 ${f.size}` : ''}</span>
      <span><button className="link" onClick={() => openCompany(f.companyId)}>{f.company}</button><em className={f.mine ? 'mine' : ''}>{f.group || '无控制集团'}</em></span>
      {line && <span className="line-brief">{f.lines?.length ?? 1} 条产线 · 班组 {count(line.heads)}/{count(line.required)} · 熟练度 {percent(line.effective)}</span>}
      {!!f.lines?.length && <LineBriefs lines={f.lines} />}
      {ratio !== undefined && <UtilBars ratio={ratio} />}
      {line?.limit && <span className="line-limit">受限：{line.limit}</span>}
      {f.project && <span className="quiet">工程：{f.project.name} · {f.project.phase}</span>}
      <button className="link open-facility" onClick={() => openModal({ kind: 'facility', id: f.id })}>{line ? '打开产线面板' : '打开设施面板'}</button>
    </li>;
  })}</ul>;
}

function BuildHere({ plot, options }: { plot: string; options: BuildOption[] }) {
  const openModal = useUIStore(s => s.openModal);
  const [open, setOpen] = useState(false);
  if (!options.length) return null;
  return <section className="archive-section"><h2><Term name="在此新建">只能建自己业务的设施；别的行业要在集团里另开姊妹公司。新建按节点付款：开工 30%、过半 40%、完工 30%，开工时掷出超支与延误。</Term></h2>
    <button className="action" onClick={() => openModal({ kind: 'construct', plot })}>打开新建面板</button>
    <button className="link" onClick={() => setOpen(!open)}>{open ? '收起' : `${options.length} 种可建`}</button>
    {open && <ul className="option-list">{options.map(o => <li key={o.companyId + o.type}><span><b>{o.company} · {o.typeName}</b><QuoteText q={o.quote} /></span>
      {o.quote.ok && <button className="link" onClick={() => openModal({ kind: 'construct', plot, company: o.companyId })}>规划</button>}</li>)}</ul>}
  </section>;
}

function GroupPicker({ world }: { world: World }) {
  return <div className="scenario-picker" role="dialog" aria-label="选择集团">
    <section className="picker-panel painted"><div className="panel-content">
      <small>剧本 · 选择你扮演的意志</small>
      <h1>{world.scenario?.name || '新的战役'}</h1>
      <p className="intro">{world.scenario?.description}</p>
      <div className="group-grid">{(world.groupChoices ?? []).map(g => <article key={g.id} className="group-card">
        <h2>{g.name}</h2>
        <p>{g.description}</p>
        <dl><dt>根据地</dt><dd>{g.home}</dd><dt>家族资金</dt><dd>{pounds(g.familyCash)}</dd><dt>旗下公司</dt><dd>{g.companies.join('、') || '—'}</dd></dl>
        <button onClick={() => command('session.choose_group', g.id)}>以此集团开局</button>
      </article>)}</div>
      <p className="footnote">集团与公司是示意数据（mock），用来验证控制与持股规则，不代表史实人物或企业。</p>
    </div></section>
  </div>;
}

/** 天下（HierarchyUI.md 第 8 节，第一步）：the place clicked — plot, 集散区 or country — with its people, markets and works. */
function PlaceArchive({ world }: { world: World }) {
  const openModal = useUIStore(s => s.openModal);
  const g = world.geography;
  const time = world.time;
  const selectionType = world.selection?.type ?? 'province';
  const countrySelection = selectionType === 'country';
  const marketSelection = selectionType === 'market';
  const noun = domainNoun[selectionType];
  const heading = world.route ? '航路档案' : `${noun}档案`;
  const title = world.route?.name || world.selection?.name || (countrySelection ? '归属待校订' : marketSelection ? '未设集散区' : g.province || '地理档案');
  // The way into the place's goods, at the top: the plot's competition, or for a 集散区 its hub's (with the 集散区's own lists).
  const marketPlot = countrySelection || world.route ? undefined : marketSelection ? world.marketArchive?.hubPlot
    : world.plot && world.plot.population !== undefined ? world.plot.id : undefined;
  return <div className="place-archive">
    <div className="panel-heading"><small>{heading}</small><span className="archive-year">{(time?.date ?? world.date).slice(0, 4)}</span><h1 key={title}>{title}</h1></div>
    {marketPlot && <div className="place-actions">
      <button className="brush-button" onClick={() => openModal({ kind: 'plot-market', plot: marketPlot, need: marketSelection ? 'market' : undefined })}>
        {marketSelection ? '集散区的竞品与集散仓' : '本地竞品与价格'}</button>
    </div>}
    <figure className="capture">
      <div className="capture-surface">{isInUnreal() ? <img src="inspector.imgsrc" alt="选中位置的实时三维画面" /> : <p>地表实景将在游戏中呈现</p>}</div>
      <figcaption><span>地表实景</span><span className="coordinates">{Math.abs(world.lat).toFixed(2)}°{world.lat >= 0 ? 'N' : 'S'}<em> / </em>{Math.abs(world.lon).toFixed(2)}°{world.lon >= 0 ? 'E' : 'W'}</span></figcaption>
    </figure>
    {world.route && <section className="route-archive"><h2>{world.route.enabled ? '可通航航路' : '未启用航路'}</h2><p>{world.route.description}</p>{!world.route.enabled && <strong>尚未开通 · 不参与通行</strong>}{world.route.historicalOpening && <p>历史通航时间：{world.route.historicalOpening}</p>}</section>}
    {countrySelection ? <section className="archive-section"><h2>领土概览</h2><dl>
      <dt>国家／政体</dt><dd>{world.selection?.name || '归属待校订'}</dd>
      <dt>所属地图地块</dt><dd>{world.selection?.provinceCount ?? 0} 块</dd>
      <dt>观察范围</dt><dd>全部领土</dd>
    </dl></section> : marketSelection ? <>
      <section className="archive-section"><h2>集散区概览</h2><dl>
        <dt><Term name="集散区">一个枢纽城市，加上周边不靠铁路和航运就能送到货的地块：车马、驮队与内河小船的中程运输范围。商品先进集散仓，区内地块从这里买。</Term></dt><dd>{world.selection?.name || '未设集散区'}</dd>
        <dt>国家／政体</dt><dd>{world.selection?.country || g.country || '归属待校订'}</dd>
        <dt>枢纽城市</dt><dd>{world.selection?.hub || '待定'}</dd>
        <dt>包含地块</dt><dd>{world.selection?.provinceCount ?? 0} 块</dd>
        {!!world.marketArchive?.population && <><dt><Term name="人口">1844 年人口，按 HYDE／ISIMIP 的 1850 年网格与 OWID 国别序列推回 1844 年；只计仍属本政权的地块。</Term></dt><dd>{people(world.marketArchive.population)}</dd></>}
      </dl></section>
      {world.marketArchive && <MarketPreview archive={world.marketArchive} />}
      <section className="archive-section"><h2>区内经营</h2>{world.marketArchive?.companies.length ? <ul className="facility-list">{world.marketArchive.companies.map(c => <li key={c.companyId}>
        <span><button className="link" onClick={() => openCompany(c.companyId)}>{c.company}</button><em className={c.mine ? 'mine' : ''}>{c.group || '无控制集团'}</em></span>
        <span>{c.facilities.join('、')}</span></li>)}</ul> : <p className="quiet">区内还没有任何公司的设施。</p>}</section>
    </> : <>
      <section className="archive-section"><h2>行政与通行</h2><dl>
        <dt>行政州</dt><dd>{g.state || '待校订'}</dd>
        <dt>国家／政体</dt><dd>{g.country || '待校订'}</dd>
        <dt>陆地相邻</dt><dd>{g.neighborCount ?? 0} 块<span className="detail-note">相邻即可通行</span></dd>
      </dl></section>
      <section className="archive-section"><h2>需求</h2><dl>
        <dt><Term name="集散区">地块从所属集散区的集散仓买东西；跨集散区只能靠铁路和航运。</Term></dt><dd>{world.plot?.market || g.market || '未设集散区'}</dd>
        <dt><Term name="人口">地块是需求边界：人口、收入三档、道路阻力与本地服务覆盖决定这里的家庭能买到什么。人口按 HYDE／ISIMIP 网格推回 1844 年；收入分档仍是示意数据，暂不显示。</Term></dt><dd>{world.plot?.population !== undefined ? people(world.plot.population) : world.plot?.demandPending ? '数据尚未接入' : '—'}</dd>
        {world.plot?.culture && <><dt><Term name="文化">地块上的人。文化以后会决定很多东西；眼下只决定家庭更爱吃哪种主粮（南米北面）：买口粮时，本地文化的主粮多一份口味加成，价钱差得多了才换。文化随存档保存，只在局中改变。</Term></dt><dd>{world.plot.culture.name}<span className="detail-note">{world.plot.culture.group} · 爱吃{world.plot.culture.staple}</span></dd></>}
      </dl></section>
      <section className="archive-section"><h2><Term name="本地设施">地块没有主人。这里列出位于此地的设施，以及拥有它们的经营公司和控制那家公司的集团。</Term></h2>
        <FacilityList facilities={world.plot?.facilities ?? []} empty="这里还没有任何公司的设施。" /></section>
      {!!world.plot?.crops?.length && <Farmers crops={world.plot.crops} />}
      {!!world.plot?.resourcePoints.length && <Deposits deposits={world.plot.resourcePoints} />}
      {world.plot && <BuildHere plot={world.plot.id} options={world.plot.build ?? []} />}
    </>}
    <p className="footnote">1844 年边界校订稿<br />滚轮改变观察层级，点击{noun}更新档案。</p>
  </div>;
}

/** 右侧书册（HierarchyUI.md 第 5、8 节）：经营（the business tree）, 商品（the goods and their 供需）, 世界（places）. */
const books = [{ id: 'business', name: '经营', label: '经营树' }, { id: 'goods', name: '商品', label: '商品书册' }, { id: 'world', name: '世界', label: '世界书册' }] as const;

function RightDock({ world }: { world: World }) {
  const dock = useUIStore(s => s.dock);
  const setDock = useUIStore(s => s.setDock);
  return <aside className="right-dock" aria-label={books.find(b => b.id === dock)?.label}>
    <NineSlice art={panelArt.dockRight} className="right-dock-surface">
      <nav className="dock-tabs">{books.map(b =>
        <button key={b.id} className={dock === b.id ? 'active' : ''} aria-pressed={dock === b.id} onClick={() => setDock(b.id)}>{b.name}</button>)}
      </nav>
      <div className="dock-scroll">{dock === 'business' ? <BusinessTree world={world} /> : dock === 'goods' ? <GoodsBook world={world} />
        : <><WorldTree world={world} />{world.countryArchive && <CountryArchive c={world.countryArchive} />}</>}</div>
    </NineSlice>
  </aside>;
}

/** 设置：the old settings page as a dialog from the menu. */
function SettingsDialog({ world, connected, onClose }: { world: World; connected: boolean; onClose: () => void }) {
  return <Modal eyebrow="表现设置" title="氛围与操作" onClose={onClose} className="settings-dialog">
    <label className="atmosphere-setting"><span>界面动态氛围<small>面板入场与笔触的轻微流动</small></span><input type="checkbox" disabled={!connected} checked={world.atmosphere} onChange={e => sendToUE('set_atmosphere', { enabled: e.target.checked })} /></label>
    <section className="archive-section"><h2>界面缩放</h2><div className="scale-options" role="group" aria-label="界面缩放">{uiScales.map(s => <button key={s.value} disabled={!connected} className={(world.uiScale ?? 0) === s.value ? 'active' : ''} aria-pressed={(world.uiScale ?? 0) === s.value} onClick={() => sendToUE('set_ui_scale', { scale: s.value })}>{s.name}</button>)}</div>
      <p className="quiet">自动：按窗口大小放大，4K 约为 200%。当前 {Math.round((world.uiScale || autoScale()) * 100)}%。</p></section>
    <section className="archive-section"><h2>地图操作</h2><dl className="key-bindings"><dt>观察层级</dt><dd>鼠标滚轮</dd><dt>平移地图</dt><dd>WASD / 中键拖动</dd><dt>水平旋转</dt><dd>按住右键</dd><dt>检查地表</dt><dd>鼠标左键</dd><dt>暂停／继续</dt><dd>空格</dd><dt>往上一层</dt><dd>Esc</dd></dl></section>
    <section className="archive-section"><h2>存档</h2><div className="save-actions"><button disabled={!connected} onClick={() => sendToUE('quick_save')}>保存 <kbd>F5</kbd></button><button disabled={!connected} onClick={() => sendToUE('quick_load')}>读取 <kbd>F9</kbd></button></div><p role="status" className="save-status">{world.saveStatus}</p></section>
    <p className="footnote">界面偏好保存在这台设备上。<br />地图资料：OpenHistoricalMap 贡献者（CC0／CC BY 4.0）、Natural Earth（公有领域）。历史边界仍在校订。</p>
  </Modal>;
}

function App() {
  const [world, setWorld] = useState<World>({ mapMode: 'market', seaRoutes: true, date: '1844.01.01', week: 0, lon: -.12, lat: 51.5, atmosphere: true, geography: { status: 'unassigned' }, markets: [] });
  const [connected, setConnected] = useState(false);
  const modal = useUIStore(s => s.modal);
  const openModal = useUIStore(s => s.openModal);
  const siting = useUIStore(s => s.siting);
  const setSiting = useUIStore(s => s.setSiting);
  const setScale = useUIStore(s => s.setScale);
  const dock = useUIStore(s => s.dock);
  const dossier = useUIStore(s => s.dossier);
  const settings = useUIStore(s => s.settings);
  const setSettings = useUIStore(s => s.setSettings);
  const placeOpen = useUIStore(s => s.placeOpen);
  const setPlaceOpen = useUIStore(s => s.setPlaceOpen);
  const [menu, setMenu] = useState(false);
  const time = world.time;
  const choosing = connected && !world.playerGroup;
  // 订阅：each message carries the core (time, selection, map) and only the views that changed; views the engine no
  // longer sends (a closed dossier, another panel) are named in `gone`.
  const views = useRef<Record<string, unknown>>({});
  useEffect(() => {
    const stop = onUECall('setWorld', d => {
      const m = d as { core?: World; views?: Record<string, unknown>; gone?: string[] };
      if (!m.core) { setWorld(d as World); setConnected(true); return; }
      for (const name of m.gone ?? []) delete views.current[name];
      Object.assign(views.current, m.views ?? {});
      setWorld({ ...m.core, ...views.current } as World);
      setConnected(true);
    });
    sendToUE('ui_ready');
    return stop;
  }, []);
  // The map page is the only page: the group is the business tree's root (HierarchyUI.md).
  useEffect(() => { if (connected) sendToUE('ui_panel', { panel: 'world' }); }, [connected]);
  useEffect(() => { document.body.classList.toggle('still', !world.atmosphere); }, [world.atmosphere]);
  const [viewport, setViewport] = useState(0);
  useEffect(() => { const resize = () => setViewport(v => v + 1); window.addEventListener('resize', resize); return () => window.removeEventListener('resize', resize); }, []);
  useEffect(() => { setScale(world.uiScale ? world.uiScale : autoScale()); }, [world.uiScale, viewport, setScale]);
  useEffect(() => { if (connected) sendToUE('ui_book', { book: dock }); }, [dock, connected]);
  // The place's archive on the left gives way to a workbench (construction, the plot's competition) and comes back after it;
  // the live 3D capture shows with it.
  const placeShown = placeOpen && !settings && !modal;
  useEffect(() => { sendToUE('inspector_visible', { visible: placeShown }); }, [placeShown]);
  // A click on the ground or in the world tree (a focus moving the camera does not count) opens the place's archive on the left;
  // the right dock and the focus stay as they are. Building and siting open their workbench instead.
  const clicks = useRef<number | undefined>(undefined);
  useEffect(() => {
    if (world.clickSerial === undefined) return;
    // The first snapshot only sets the count: the game opens on the business tree.
    const ui = useUIStore.getState();
    if (clicks.current !== undefined && world.clickSerial !== clicks.current && !world.build && !ui.siting) setPlaceOpen(true);
    clicks.current = world.clickSerial;
  }, [world.clickSerial, world.build, setPlaceOpen]);
  // For the in-engine smokes: the old rail's pages as one call (group = the group's dossier, world = the map clear of
  // panels with the world book and the place's archive, settings = the settings dialog); a focus as the trees would pick it.
  useEffect(() => {
    (window as unknown as { __bmPage: (page: string) => void }).__bmPage = (page: string) => {
      const ui = useUIStore.getState();
      if (page === 'group') { if (world.playerGroup) focusOn('group', world.playerGroup); ui.closeModal(); ui.openDossier(); ui.setDock('business'); }
      else if (page === 'settings') ui.setSettings(true);
      else { ui.closeModal(); ui.closeDossier(); ui.setSettings(false); ui.setPlaceOpen(true); ui.setDock('world'); }
    };
    (window as unknown as { __bmOpen: (kind: string, id: string, tab?: string) => void }).__bmOpen = (kind: string, id: string, tab?: string) => {
      const ui = useUIStore.getState();
      if (kind === 'company') ui.openModal({ kind: 'company', id, tab });
      else if (kind === 'facility') ui.openModal({ kind: 'facility', id, tab });
      else if (kind === 'product' || kind === 'brand') ui.openModal({ kind: 'product', subject: kind, id, tab });
      else if (kind === 'good') { focusOn('good', id); ui.openDossier(); ui.setDock('goods'); }
    };
    (window as unknown as { __bmFocus: (kind: string, id: string, view?: string) => void }).__bmFocus = (kind: string, id: string, view?: string) => {
      focusOn(kind as TreeKind, id, view);
      if (kind === 'good') useUIStore.getState().setDock('goods');
    };
  }, [world.playerGroup]);
  // 历史：Alt+← / Alt+→ and the mouse's back and forward buttons.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (!e.altKey || (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight')) return;
      e.preventDefault();
      sendToUE('navigate', { dir: e.key === 'ArrowLeft' ? 'back' : 'forward' });
    };
    const onMouse = (e: MouseEvent) => { if (e.button === 3 || e.button === 4) sendToUE('navigate', { dir: e.button === 3 ? 'back' : 'forward' }); };
    window.addEventListener('keydown', onKey);
    window.addEventListener('mouseup', onMouse);
    return () => { window.removeEventListener('keydown', onKey); window.removeEventListener('mouseup', onMouse); };
  }, []);
  // Esc with no panel open closes the place's archive, else goes up one level of the business tree.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== 'Escape' || modal || dossier || settings) return;
      if (useUIStore.getState().placeOpen) { setPlaceOpen(false); return; }
      const path = world.focus?.path ?? [];
      if (path.length > 1) { const up = path[path.length - 2]; focusOn(up.kind, up.id); }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [world.focus, modal, dossier, settings, setPlaceOpen]);
  // 选址：the plot clicked while choosing a site opens the construction panel.
  const plotId = world.plot?.id;
  useEffect(() => {
    if (siting && plotId) openModal({ kind: 'construct', plot: plotId, company: siting.company });
    // 建筑：the build view opens the panel for its facility type and company.
    else if (world.build && plotId) openModal({ kind: 'construct', plot: plotId, company: world.build.company, type: world.build.type });
    // Only a new plot selection triggers it.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [plotId]);
  const share = world.shareFocus;
  const freightShown = world.mapMode === 'market_share' && (share?.kind === 'good' || share?.kind === 'product') && share.physical;
  const player = world.player;

  return <>
    <header className="top">
      <div className="identity dock-surface" data-paint-anchor="top-left"><div className="identity-content">
        <div className="brand"><b>血钱</b><small>BLOOD MONEY</small></div>
        {world.playerGroup && player ? <button className="group-strip" onClick={() => focusOn('group', world.playerGroup!)}>
          <b>{player.name}</b><span>家族资金 {player.familyCash === undefined ? '—' : shortPounds(player.familyCash)}</span></button>
          : <div className="chapter">资本的世纪<span>意志 · 法人 · 世界</span></div>}
      </div></div>
      <div className="clock dock-surface" data-paint-anchor="top-right"><div className="clock-content">
        <div className="date"><small>{time?.halted ? '结算暂停 · 账目不平' : time?.paused ? '已暂停' : `速度 ${time?.speed ?? 1}`}</small><strong>{time?.date ?? world.date}</strong></div>
        <button disabled={!connected || choosing} onClick={() => command('time.toggle_pause')}>{time && !time.paused ? 'Ⅱ 暂停' : '▶ 继续'}</button>
        <div className="speeds" role="group" aria-label="速度">{[1, 2, 3, 4, 5].map(s => <button key={s} disabled={!connected || choosing} aria-pressed={time?.speed === s} className={time?.speed === s ? 'active' : ''} aria-label={`速度 ${s}`} onClick={() => command('time.set_speed', '', { speed: String(s) })}><i style={{ height: 5 + s * 3 }} /></button>)}</div>
        <button className="menu-button" aria-label="菜单" aria-expanded={menu} onClick={() => setMenu(!menu)}>≡</button>
      </div></div>
      {menu && <div className="top-menu" role="menu">
        <button role="menuitem" disabled={!connected} onClick={() => { sendToUE('quick_save'); setMenu(false); }}>保存 <kbd>F5</kbd></button>
        <button role="menuitem" disabled={!connected} onClick={() => { sendToUE('quick_load'); setMenu(false); }}>读取 <kbd>F9</kbd></button>
        <button role="menuitem" onClick={() => { setSettings(true); setMenu(false); }}>设置</button>
      </div>}
    </header>

    {!choosing && <FloatingUIOverlay hidden={settings} showStats={world.mapMode === 'market_share' && !world.routeView} routeView={!!world.routeView}
      roles={world.focus?.kind === 'good' && world.focus.view === 'supply'} focusKind={world.focus?.kind}
      openPlot={placeOpen && (world.selection?.type ?? 'province') === 'province' ? world.plot?.id : undefined} />}

    {!choosing && <RightDock world={world} />}
    {!choosing && placeShown && <PlaceDock world={world}><PlaceArchive world={world} /></PlaceDock>}
    {!choosing && <DossierHost world={world} />}
    {!choosing && <WorkbenchHost world={world} />}
    {!choosing && <ViewStrip world={world} connected={connected} />}
    {!choosing && <AlertBar alerts={world.alerts ?? []} />}
    {freightShown && <div className="goods-hint">地块的价格都是到户价：货架价加从集散仓送到这个地块的费用。主力商品是这个集散区上周卖得最多的一款；线上标的是每{share?.unit || '单位'}运价；指向一个集散区，各地块标出两个价格。</div>}
    {settings && <SettingsDialog world={world} connected={connected} onClose={() => setSettings(false)} />}
    {!siting && <BuildBanner world={world} />}
    {siting && <div className="siting-banner" role="status"><b>为{siting.name}选址</b><span>{world.cameraTier === 'near' || world.cameraTier === 'mid' ? '点击一个地块，打开新建面板。' : '先放大到地块层级，再点击地块。'}</span><button className="link" onClick={() => setSiting(null)}>取消</button></div>}
    {choosing && !!world.groupChoices?.length && <GroupPicker world={world} />}
    {time?.lastCommand && !time.lastCommand.ok && <div className="command-toast" role="status">{time.lastCommand.message}</div>}
    <footer><span>WASD 平移 <em>·</em> 中键拖图 <em>·</em> 右键旋转 <em>·</em> 滚轮缩放 <em>·</em> 空格暂停 <em>·</em> Esc 往上一层</span><span role="status">{world.saveStatus || 'F5 保存 · F9 读取'}</span></footer>
  </>;
}
// A neutral preview canvas makes alpha edges inspectable without faking campaign data.
if (!isInUnreal()) document.documentElement.classList.add('browser-preview');
createRoot(document.getElementById('root')!).render(<TooltipProvider><App /></TooltipProvider>);
