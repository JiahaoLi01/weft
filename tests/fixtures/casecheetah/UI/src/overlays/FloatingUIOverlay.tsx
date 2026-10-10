import { Fragment } from 'react';
import { Hoverable, getLayoutRect, getZoom } from '@/components/tooltip/TooltipSystem';
import { count, decimal, facilityGlyph, focusFacility, formatMeasure, measureLabel, measureNames, percent, pounds, price, statusTone } from '@/lib/format';
import { useFloatingUIStore, type FloatingInstance } from '@/stores/floating-ui-store';
import { useUIStore } from '@/stores/ui-store';
import { emblemArt } from '@/lib/emblems';

/** What a facility marker says on hover. */
function FacilityTip({ d }: { d: Record<string, string> }) {
  return <div className="marker-tip">
    <b>{d.name}</b>
    <span>{d.type}{Number(d.size) > 1 ? ` · 规模 ${d.size}` : ''}</span>
    <span>{d.company}{d.group ? ` · ${d.group}` : ''}</span>
    <span className={`tone-${statusTone(d.status)}`}>{d.status}</span>
    {d.output && <span>昨日 {d.output}</span>}
    <em>点击打开{d.production === '1' ? '产线' : '设施'}面板</em>
  </div>;
}

function FacilityChip({ instance }: { instance: FloatingInstance }) {
  const openModal = useUIStore(s => s.openModal);
  const d = instance.data;
  const lit = useUIStore(s => s.preview.includes(d.facility));
  const ratio = d.ratio !== undefined ? Number(d.ratio) : undefined;
  return <Hoverable className="chip-hit" content={<FacilityTip d={d} />} pinnable={false} width={240}>
    <button className={`marker-chip ${d.mine === '1' ? 'mine' : ''} ${statusTone(d.status)} ${d.focus ? `focus-${d.focus}` : ''} ${lit ? 'preview' : ''} ${d.role ? `role-${d.role}` : ''}`}
      data-facility={d.facility} onClick={() => openModal({ kind: 'facility', id: d.facility })} aria-label={`${d.name}：${d.status}`}>
      {emblemArt(d.typeId) ? <img className="chip-emblem" src={emblemArt(d.typeId)} alt="" /> : <i className="glyph">{facilityGlyph[d.typeId] ?? d.type.slice(0, 1)}</i>}
      <span className="chip-name">{d.name}</span>
      {ratio !== undefined && <span className="chip-meter" aria-hidden="true"><i style={{ width: `${Math.max(0, Math.min(1, ratio)) * 100}%` }} /></span>}
      {d.project === '1' && <span className="chip-flag">工</span>}
    </button>
  </Hoverable>;
}

/** Far views: one pin per plot; a click flies the camera there. */
function CompactPin({ items }: { items: FloatingInstance[] }) {
  const mine = items.some(i => i.data.mine === '1');
  const lit = useUIStore(s => items.some(i => s.preview.includes(i.data.facility)));
  const first = items[0].data;
  const limited = items.some(i => statusTone(i.data.status) === 'limited');
  return <Hoverable className="chip-hit" pinnable={false} width={240} content={<div className="marker-tip">{items.map(i => <span key={i.id}><b>{i.data.name}</b> · {i.data.status}{i.data.ratio ? ` · ${percent(Number(i.data.ratio))}` : ''}</span>)}<em>点击飞到这里</em></div>}>
    <button className={`marker-pin-compact ${mine ? 'mine' : ''} ${limited ? 'limited' : ''} ${lit ? 'preview' : ''}`} onClick={() => focusFacility(first.facility)} aria-label={`${items.length} 处设施`}>
      {emblemArt(first.typeId) ? <img className="chip-emblem" src={emblemArt(first.typeId)} alt="" /> : <i className="glyph">{facilityGlyph[first.typeId] ?? first.type.slice(0, 1)}</i>}{items.length > 1 && <span>+{items.length - 1}</span>}
    </button>
  </Hoverable>;
}

const statMeasures = ['share', 'units', 'revenue', 'delivered', 'main', 'price', 'stock', 'sellers', 'awareness', 'loyalty'] as const;
/** A good's figures read in its own unit (units and stock of a good, not need units). */
const statText = (d: Record<string, string>, m: string) => d.kind === 'good' && (m === 'units' || m === 'stock') ? `${count(Number(d[m]))} ${d.unit ?? ''}` : formatMeasure(m, Number(d[m]));
/** A chip's approximate footprint (logical px) for decluttering: CJK names, then the figure. */
const chipWidth = (d: Record<string, string>) => Math.min(170, 24 + d.name.length * 10 + statText(d, d.measure).length * 6.5);

/** 焦点对象在一个集散区的数值签：当前指标的数值；悬停看全部指标与最大对手；点击打开档案并定位到这个集散区。 */
function StatChip({ instance, dot }: { instance: FloatingInstance; dot?: boolean }) {
  const openModal = useUIStore(s => s.openModal);
  const d = instance.data;
  const text = statText(d, d.measure);
  const good = d.kind === 'good';
  const open = () => good ? (d.hubPlot ? openModal({ kind: 'plot-market', plot: d.hubPlot }) : undefined)
    : d.kind === 'company' ? openModal({ kind: 'company', id: d.subject, tab: 'products' })
    : openModal({ kind: 'product', subject: d.kind as 'product' | 'brand', id: d.subject, market: d.market, tab: 'markets' });
  return <Hoverable className="chip-hit" pinnable={false} width={250} content={<div className="marker-tip">
    <b>{d.name}</b>
    {good ? <>
      {Number(d.main) >= 0 && <span>主力商品价格 {price(Number(d.main))}<em>{d.mainName} · 货架 {price(Number(d.mainShelf))} + 送到枢纽 {price(Number(d.fee))}</em></span>}
      {Number(d.delivered) >= 0 && <span>选中商品到户价格 {price(Number(d.delivered))}<em>{d.selectedName} · 货架 {price(Number(d.selectedShelf))}</em></span>}
      {d.selectedName && Number(d.delivered) < 0 && <span>选中商品 {d.selectedName} 这里没货</span>}
      {(['units', 'stock', 'sellers'] as const).map(m => Number(d[m]) > 0 ? <span key={m}>{measureNames[m]} {statText(d, m)}</span> : null)}
    </> : statMeasures.map(m => d[m] !== undefined && (Number(d[m]) > 0 || (m === 'share' && Number(d.share) >= 0)) ? <span key={m}>{measureLabel(d.kind, m)} {statText(d, m)}</span> : null)}
    {!good && d.mainName && Number(d.main) >= 0 && <span>主力商品：{d.mainName}</span>}
    {d.rival && <span>最大对手：{d.rival}</span>}
    <em>{good ? '点击看枢纽地块的竞品与价格' : '点击打开档案'}</em>
  </div>}>
    <button className={`stat-chip measure-${d.measure} ${dot ? 'dot' : ''}`} onClick={open} aria-label={`${d.name} ${measureLabel(d.kind, d.measure)} ${text}`}>
      {!dot && <><span className="stat-name">{d.name}</span><b>{text}</b></>}
    </button>
  </Hoverable>;
}

const modeGlyph: Record<string, string> = { sea: '船', rail: '铁', river: '河', road: '驮', air: '空' };
const cardRank = (i: FloatingInstance) => i.data.mode === 'rail' ? 0 : i.data.mode === 'air' ? 1 : 2;
/** Lines without an operating company are carried outside the model. */
const outsideCarrier: Record<string, string> = { sea: '模型外的船行', rail: '模型外的铁路公司', river: '模型外的民船', road: '模型外的驮帮与车行' };

/** 运价签：一条线上的每家承运人，每单位所看商品的运费、班期与舱位；签上的数字是经销商今天会订的那家。 */
function RouteChip({ instance, dot }: { instance: FloatingInstance; dot?: boolean }) {
  const d = instance.data;
  const freight = Number(d.freight);
  const chosen = Number(d.chosen ?? -1);
  const carriers = Array.from({ length: Number(d.carriers ?? 0) }, (_, k) => ({
    name: d[`c${k}name`], group: d[`c${k}group`], fare: Number(d[`c${k}fare`]), perTon: Number(d[`c${k}perTon`]),
    transit: d[`c${k}transit`], every: Number(d[`c${k}every`]), room: Number(d[`c${k}room`]) }));
  return <Hoverable className="chip-hit" pinnable={false} width={320} content={<div className="marker-tip">
    <b>{d.name}</b>
    <span>{d.modeName || d.mode} · {count(Number(d.km))} 公里 · {carriers.length} 家承运</span>
    {carriers.map((c, k) => <span key={k} className={k === chosen ? 'chosen' : ''}>
      {k === chosen ? '▸ ' : ''}{c.name ? `${c.name}${c.group ? ` · ${c.group}` : ''}` : outsideCarrier[d.mode] ?? '模型外的承运人'}
      <em>每{d.unit} {price(c.fare)}（每吨 £{c.perTon.toFixed(2)}）· 在途 {c.transit} 天 · {c.every > 1 ? `每 ${c.every} 天一班` : '每天发运'}{c.room > 0 ? ` · 每班 ${count(c.room)} 吨` : ''}</em></span>)}
    <em>托运人按“运价 + 时间价值 × 天数”挑承运人；▸ 是{d.good}今天会订的那家。经销商按到岸价（出厂价 + 运费）再加价出售</em>
  </div>}>
    <span className={`route-chip mode-${d.mode} ${dot ? 'dot' : ''}`} aria-label={`${d.name} 运价 ${price(freight)}`}>
      {!dot && <><i>{modeGlyph[d.mode] ?? '运'}</i><b>{price(freight)}</b></>}
    </span>
  </Hoverable>;
}

/** One carrier on a line, as the route cards read it from the floating data. */
function lineServices(d: Record<string, string>) {
  return Array.from({ length: Number(d.services ?? 0) }, (_, k) => ({
    name: d[`s${k}name`], group: d[`s${k}group`], vehicles: Number(d[`s${k}vehicles`]), every: Number(d[`s${k}every`]),
    transit: Number(d[`s${k}transit`]), paxDays: Number(d[`s${k}paxDays`]), room: Number(d[`s${k}room`]), seats: Number(d[`s${k}seats`]),
    passengers: d[`s${k}passengers`] === '1', rate: Number(d[`s${k}rate`]), fare: Number(d[`s${k}fare`]), className: d[`s${k}className`], classFare: Number(d[`s${k}classFare`] ?? 0),
    carried: Number(d[`s${k}carried`]),
    offered: Number(d[`s${k}offered`]), pax: Number(d[`s${k}pax`]), seatsTaken: Number(d[`s${k}seatsTaken`]), seatsOffered: Number(d[`s${k}seatsOffered`]),
    revenue: Number(d[`s${k}revenue`]), paxRevenue: Number(d[`s${k}paxRevenue`]) }));
}
const everyText = (every: number) => every > 1 ? `每 ${every} 天一班` : '每天一班';

/** A run of card text; bold runs are figures. */
type Run = { t: string; b?: boolean };
/** Width of text at a font size: CJK about a full em, the rest about six tenths (logical px). */
const textWidth = (text: string, px: number) => [...text].reduce((w, ch) => w + (/[\u2E80-\u9FFF\uFF00-\uFFEF]/.test(ch) ? px * 1.04 : px * .6), 0);

/**
 * 远程航线视图的线路卡。铁路写满：名称、里程与班期，客运（票价、每班座位、上座率、上周人次），货运（每吨运价、每班吨位、满载率）；
 * 海运与空运只写天数与班期。悬停列出各承运公司与上周的客、货、收入；卡上不写公司名。
 */
function routeCard(d: Record<string, string>, compact: boolean) {
  const services = lineServices(d);
  const rail = d.mode === 'rail';
  const every = services.length ? Math.min(...services.map(s => s.every)) : 0;
  const days = services.length ? Math.min(...services.map(s => s.transit)) : 0;
  const riders = services.filter(s => s.passengers);
  const seats = riders.reduce((n, s) => n + s.seats, 0);
  const fare = riders.length ? Math.min(...riders.map(s => s.fare)) : 0;
  const cheap = riders.filter(s => s.className && s.classFare > 0);
  const classFare = cheap.length ? Math.min(...cheap.map(s => s.classFare)) : 0;
  const rate = services.length ? Math.min(...services.map(s => s.rate)) : 0;
  const room = services.reduce((n, s) => n + s.room, 0);
  const pax = services.reduce((n, s) => n + s.pax, 0);
  const taken = services.reduce((n, s) => n + s.seatsTaken, 0), offered = services.reduce((n, s) => n + s.seatsOffered, 0);
  const carried = services.reduce((n, s) => n + s.carried, 0), tonsOffered = services.reduce((n, s) => n + s.offered, 0);
  const name = rail ? d.name.replace(/铁路$/, '') : '';
  const side = rail ? (compact ? '' : `${count(Number(d.km))} 公里${services.length ? ` · ${everyText(every)}` : ''}`) : `${days} 天 · ${everyText(every)}`;
  const rows: Run[][] = [];
  if (rail && !services.length) rows.push([{ t: '没有在跑的列车' }]);
  else if (rail) {
    if (riders.length) rows.push(compact
      ? [{ t: '客 ' }, { t: price(fare), b: true }, ...(offered > 0 ? [{ t: ' · 上座 ' }, { t: percent(taken / offered), b: true }] : [])]
      : [{ t: '客 ' }, { t: price(fare), b: true }, ...(classFare > 0 ? [{ t: `／${cheap[0].className} ` }, { t: price(classFare), b: true }] : []), { t: ` · ${count(seats)} 座` },
         ...(offered > 0 ? [{ t: ' · 上座 ' }, { t: percent(taken / offered), b: true }] : []), { t: ` · ${count(pax)} 人次` }]);
    if (!compact) rows.push([{ t: '货 ' }, { t: price(rate), b: true }, { t: `/吨 · ${count(Math.round(room))} 吨` },
      ...(tonsOffered > 0 ? [{ t: ' · 满载 ' }, { t: percent(carried / tonsOffered), b: true }] : [])]);
  }
  const width = 14 + Math.max(15 + 5 + textWidth(name, 12) + (side ? 6 + textWidth(side, 10) : 0),
    ...rows.map(r => 20 + textWidth(r.map(x => x.t).join(''), 10)));
  const height = 19 + rows.length * 15;
  const tip = <div className="marker-tip">
    <b>{d.name}</b>
    <span>{d.modeName || d.mode} · {count(Number(d.km))} 公里{d.holder ? ` · 路权 ${d.holder}` : ''}</span>
    <span>基准：货运每吨 {price(Number(d.baseRate))} · 客票单程 {price(Number(d.baseFare))}</span>
    {services.map((s, k) => <span key={k}>{s.name ? `${s.name}${s.group ? ` · ${s.group}` : ''}` : outsideCarrier[d.mode] ?? '模型外的承运人'}
      <em>{s.vehicles ? `${count(s.vehicles)} ${rail ? '列' : '艘'} · ` : ''}{everyText(s.every)} · 货运在途 {s.transit} 天 · 每吨 {price(s.rate)}{s.passengers ? ` · 客票 ${price(s.fare)}${s.className ? `（${s.className} ${price(s.classFare)}）` : ''}、车上 ${s.paxDays} 天${s.seats ? `、每天 ${count(s.seats)} 座` : '（与货物共用舱位）'}` : ''}
        {s.name ? ` · 上周 ${count(s.pax)} 人次、${decimal(s.carried)} 吨，收入 ${price(s.revenue)}` : ''}</em></span>)}
    {!services.length && <span>这条线上没有在跑的车船。</span>}
  </div>;
  return { name, side, rows, width, height, tip };
}

function RouteCard({ instance, dot, compact }: { instance: FloatingInstance; dot?: boolean; compact: boolean }) {
  const d = instance.data;
  const card = routeCard(d, compact);
  if (dot) return <Hoverable className="chip-hit" pinnable={false} width={320} content={card.tip}><span className={`route-card dot mode-${d.mode}`} aria-label={d.name} /></Hoverable>;
  return <Hoverable className="chip-hit" pinnable={false} width={320} content={card.tip}>
    <span className={`route-card mode-${d.mode} ${d.mode === 'rail' && !compact ? 'full' : ''}`} aria-label={`${d.name} ${d.modeName ?? ''}`}>
      <span className="route-card-head"><i>{modeGlyph[d.mode] ?? '运'}</i>{card.name && <b>{card.name}</b>}{card.side && <em>{card.side}</em>}</span>
      {card.rows.map((r, k) => <span key={k} className="route-card-row">{r.map((x, j) => x.b ? <b key={j}>{x.t}</b> : <Fragment key={j}>{x.t}</Fragment>)}</span>)}
    </span>
  </Hoverable>;
}

/** 送货签：指向的集散区里，从集散仓送到每个地块的费用；点击看这个地块的竞品。 */
function PlotCostChip({ instance, dot }: { instance: FloatingInstance; dot?: boolean }) {
  const openModal = useUIStore(s => s.openModal);
  const d = instance.data;
  const fee = Number(d.fee), main = Number(d.main), delivered = Number(d.delivered);
  return <Hoverable className="chip-hit" pinnable={false} width={260} content={<div className="marker-tip">
    <b>{d.name}</b>
    {main >= 0 && <span>主力商品价格 {price(main)}<em>{d.mainName} · 货架 {price(Number(d.mainShelf))} + 送货 {price(fee)}</em></span>}
    {delivered >= 0 && <span>选中商品到户价格 {price(delivered)}<em>{d.selectedName} · 货架 {price(Number(d.selectedShelf))} + 送货 {price(fee)}</em></span>}
    {d.selectedName && delivered < 0 && <span>选中商品 {d.selectedName} 这里没货</span>}
    <span>{d.market}集散仓送到这里每{d.unit} {price(fee)} · 道路阻力 {Number(d.road).toFixed(2)}</span>
    <em>点击看这个地块的竞品与价格</em>
  </div>}>
    <button className={`plot-cost-chip ${dot ? 'dot' : ''}`} onClick={() => openModal({ kind: 'plot-market', plot: d.plot })}
      aria-label={`${d.name} 主力商品价格 ${price(main)}${delivered >= 0 ? ` 选中商品到户价格 ${price(delivered)}` : ''}`}>
      {!dot && <><span className="cost-name">{d.name}</span><b>{main >= 0 ? price(main) : '—'}</b>{delivered >= 0 && <><span className="cost-name">选中</span><b className="picked">{price(delivered)}</b></>}</>}
    </button>
  </Hoverable>;
}

/** 周边：one source of an input priced at the focused works — farmers (their ask + the carting), a seller's stock (its price + the
 *  carting from its nearest granary) or a port's imports; the cheapest of each grain gold. */
function SourceChip({ instance, dot }: { instance: FloatingInstance; dot?: boolean }) {
  const d = instance.data;
  const delivered = Number(d.delivered), quantity = Number(d.quantity);
  const cheapest = d.cheapest === '1';
  return <Hoverable className="chip-hit" pinnable={false} width={250} content={<div className="marker-tip">
    <b>{d.name} · {d.goodName}</b>
    <span>到{d.works ?? '厂'} {price(delivered)}／{d.unit}</span>
    <em>{d.kind === 'import' ? `进口价 ${price(Number(d.price))}` : `${d.kind === 'farm' ? '地头价' : '卖价'} ${price(Number(d.price))} + 车马费 ${price(Number(d.haul))}`}</em>
    {quantity >= 0 && <span>{d.kind === 'farm' ? '农户存粮' : '存货'} {count(quantity)} {d.unit}</span>}
    {cheapest && <em>这种粮现在最便宜的来源</em>}
  </div>}>
    <span className={`source-chip ${cheapest ? 'cheapest' : ''} ${dot ? 'dot' : ''}`} aria-label={`${d.name} ${d.goodName} 到厂 ${price(delivered)}`}>
      {!dot && <><img src={`art/goods/${d.good}.png`} alt="" /><span className="cost-name">{d.name}</span><b>{price(delivered)}</b></>}
    </span>
  </Hoverable>;
}

/** 集团内供需签：the good one company of the group makes and another uses, and what went between them last week. */
function LinkChip({ instance, dot }: { instance: FloatingInstance; dot?: boolean }) {
  const d = instance.data;
  const inside = Number(d.inside), outside = Number(d.outside);
  return <Hoverable className="chip-hit" pinnable={false} width={260} content={<div className="marker-tip">
    <b>{d.from} → {d.to}</b>
    <span>{d.goodName}：{d.from}做，{d.to}用</span>
    <em>上周从集团内进 {decimal(inside)} {d.unit}，从别处进 {decimal(outside)} {d.unit}</em>
  </div>}>
    <span className={`link-chip ${dot ? 'dot' : ''}`} aria-label={`${d.from}供给${d.to}：${d.goodName}`}>
      {!dot && <><img src={`art/goods/${d.good}.png`} alt="" /><span className="cost-name">{d.goodName}</span>{inside > 0 && <b>{decimal(inside)} {d.unit}</b>}</>}
    </span>
  </Hoverable>;
}

/** 进货签：where the focus's lines bought, how much, and what it came to a unit (the carting included). */
function BuyChip({ instance, dot }: { instance: FloatingInstance; dot?: boolean }) {
  const d = instance.data;
  const quantity = Number(d.quantity), haul = Number(d.haul);
  return <Hoverable className="chip-hit" pinnable={false} width={250} content={<div className="marker-tip">
    <b>{d.name} · {d.goodName}</b>
    <span>{d.period}进 {decimal(quantity)} {d.unit}，共 {pounds(Number(d.cost))}</span>
    <em>每{d.unit} {price(Number(d.perUnit))}{haul > 0 && quantity > 0 ? `，其中车马费 ${price(haul / quantity)}` : ''}</em>
  </div>}>
    <span className={`buy-chip ${dot ? 'dot' : ''}`} aria-label={`${d.name} ${d.goodName} ${decimal(quantity)} ${d.unit}`}>
      {!dot && <><img src={`art/goods/${d.good}.png`} alt="" /><b>{decimal(quantity)} {d.unit}</b><span className="cost-name">{d.name}</span></>}
    </span>
  </Hoverable>;
}

/** Focus kinds that stand for one company: its buildings alone are marked. */
const companyFocus = new Set(['company', 'brand', 'product', 'facility', 'line']);

/**
 * 地图上的设施浮标。UE 投影每处设施（所在地块的锚点），这里按地块叠成一列。近景、中景显示名字与昨日产能利用，
 * 地图集层级只留一个点；战略层级不显示。模态面板打开或离开舆图页时整层隐藏。
 * 只标焦点的设施：焦点是一家公司（或它的建筑、品牌、产品）时只标这家公司的建筑，否则标经营树的集团；左侧打开的地块上，
 * 谁家的都标。全世界的设施一起标会盖住地图上的数字。
 */
export function FloatingUIOverlay({ hidden, showStats, routeView, roles, focusKind, openPlot }: { hidden: boolean; showStats: boolean; routeView?: boolean; roles?: boolean;
  focusKind?: string; openPlot?: string }) {
  const tier = useFloatingUIStore(s => s.tier);
  const instances = useFloatingUIStore(s => s.instances);
  const positions = useFloatingUIStore(s => s.positions);
  const markerMode = useUIStore(s => s.markerMode);
  useUIStore(s => s.uiScale);
  if (hidden || tier === 'strategic') return null;
  const z = getZoom();
  const vw = window.innerWidth / z, vh = window.innerHeight / z;
  const compact = tier === 'midfar' || tier === 'far';
  const groups = new Map<string, FloatingInstance[]>();
  // Value chips of the market-metrics focus: the best markets first, fewer in the far views.
  const stats = showStats ? instances.filter(i => i.type === 'market-stat').sort((a, b) => Number(a.data.rank) - Number(b.data.rank)).slice(0, tier === 'far' ? 12 : 40) : [];
  const placed: { l: number; r: number; t: number; b: number }[] = [];
  // 商品地图：送货签（指向的集散区）与运价签，排在数值签之后。
  const costs = showStats ? instances.filter(i => i.type === 'plot-cost') : [];
  const routes = showStats ? instances.filter(i => i.type === 'route-price') : [];
  // 焦点的周边与进货签（HierarchyUI.md 第 4 节）：purchases first, then the sources, cheapest first.
  const buys = routeView ? [] : instances.filter(i => i.type === 'buy');
  const links = routeView ? [] : instances.filter(i => i.type === 'link');
  const sources = routeView ? [] : instances.filter(i => i.type === 'source').sort((a, b) => Number(a.data.rank) - Number(b.data.rank));
  const place = (x: number, y: number, w: number, h: number) => {
    const box = { l: x - w / 2, r: x + w / 2, t: y - h, b: y };
    const clear = !placed.some(p => box.l < p.r && box.r > p.l && box.t < p.b && box.b > p.t);
    if (clear) placed.push(box);
    return clear;
  };
  // 远程航线视图：only the lines' cards (railways first, then air and sea); no facility markers, no company names. The open
  // panels count as taken, so no card hides under them.
  const cards = routeView ? instances.filter(i => i.type === 'route-info').sort((a, b) => cardRank(a) - cardRank(b) || a.data.name.localeCompare(b.data.name)) : [];
  if (routeView)
    for (const panel of document.querySelectorAll('.inspector, aside.markets, nav.rail, .map-controls, .clock, .brand')) {
      const r = getLayoutRect(panel.getBoundingClientRect());
      if (r.width > 0 && r.height > 0) placed.push({ l: r.left, r: r.right, t: r.top, b: r.bottom });
    }
  const oneCompany = !!focusKind && companyFocus.has(focusKind);
  for (const i of instances) {
    if (routeView || markerMode === 'off' || i.type !== 'facility') continue;
    // A good's 供需: its makers and users, whoever's they are. Else the focus's own (UE marks the focused company's buildings
    // focus-company / focus-facility, the tree's group root) and everyone's on the plot open on the left.
    const own = oneCompany ? i.data.focus === 'company' || i.data.focus === 'facility' : i.data.root === '1';
    if (roles ? !i.data.role : !own && i.data.plot !== openPlot) continue;
    groups.set(i.data.plot, [...(groups.get(i.data.plot) ?? []), i]);
  }
  return <div className="floating-layer" aria-label="设施浮标">
    {[...groups.entries()].map(([plot, items]) => {
      const pos = positions[items[0].id];
      if (!pos || pos.behind || !pos.onScreen) return null;
      items.sort((a, b) => (b.data.mine === '1' ? 1 : 0) - (a.data.mine === '1' ? 1 : 0) || a.data.name.localeCompare(b.data.name));
      return <div key={plot} className={`marker-stack ${compact ? 'compact' : ''}`} style={{ left: pos.nx * vw, top: pos.ny * vh }}>
        {compact ? <CompactPin items={items} /> : items.map(i => <FacilityChip key={i.id} instance={i} />)}
        <i className="marker-stem" aria-hidden="true" />
      </div>;
    })}
    {links.map(i => {
      const pos = positions[i.id];
      if (!pos || pos.behind || !pos.onScreen) return null;
      const x = pos.nx * vw, y = pos.ny * vh;
      const clear = place(x, y + 9, 46 + i.data.goodName.length * 10 + (Number(i.data.inside) > 0 ? 7 * 6.5 : 0), 20);
      return <div key={i.id} className={`cost-anchor ${clear ? '' : 'dot-only'}`} style={{ left: x, top: y }}><LinkChip instance={i} dot={!clear} /></div>;
    })}
    {buys.map(i => {
      const pos = positions[i.id];
      if (!pos || pos.behind || !pos.onScreen) return null;
      const x = pos.nx * vw, y = pos.ny * vh;
      const clear = place(x, y + 9, 46 + (i.data.name.length + 2) * 10 + 6 * 6.5, 20);
      return <div key={i.id} className={`cost-anchor ${clear ? '' : 'dot-only'}`} style={{ left: x, top: y }}><BuyChip instance={i} dot={!clear} /></div>;
    })}
    {sources.map(i => {
      const pos = positions[i.id];
      if (!pos || pos.behind || !pos.onScreen) return null;
      const x = pos.nx * vw, y = pos.ny * vh;
      const clear = place(x, y + 9, 40 + i.data.name.length * 10 + 7 * 6.5, 20);
      return <div key={i.id} className={`cost-anchor ${clear ? '' : 'dot-only'}`} style={{ left: x, top: y }}><SourceChip instance={i} dot={!clear} /></div>;
    })}
    {costs.map(i => {
      const pos = positions[i.id];
      if (!pos || pos.behind || !pos.onScreen) return null;
      // The market under the mouse: its plots' delivery costs take the space first.
      const x = pos.nx * vw, y = pos.ny * vh;
      const clear = place(x, y + 9, 30 + i.data.name.length * 10 + 7 * 6.5 + (Number(i.data.delivered) >= 0 ? 24 + 7 * 6.5 : 0), 18);
      return <div key={i.id} className={`cost-anchor ${clear ? '' : 'dot-only'}`} style={{ left: x, top: y }}><PlotCostChip instance={i} dot={!clear} /></div>;
    })}
    {stats.map(i => {
      const pos = positions[i.id];
      if (!pos || pos.behind || !pos.onScreen) return null;
      // Best-ranked first: a chip that would cover one already placed shrinks to its dot (hover still shows it).
      const x = pos.nx * vw, y = pos.ny * vh, w = chipWidth(i.data);
      const clear = place(x, y - 6, w, 21);
      return <div key={i.id} className={`stat-anchor ${clear ? '' : 'dot-only'}`} style={{ left: x, top: y }}>
        <StatChip instance={i} dot={!clear} />{clear && <i className="stat-dot" aria-hidden="true" />}
      </div>;
    })}
    {routes.map(i => {
      const pos = positions[i.id];
      if (!pos || pos.behind || !pos.onScreen) return null;
      const x = pos.nx * vw, y = pos.ny * vh;
      const clear = place(x, y + 9, 34 + price(Number(i.data.freight)).length * 6.5, 18);
      return <div key={i.id} className={`route-anchor ${clear ? '' : 'dot-only'}`} style={{ left: x, top: y }}><RouteChip instance={i} dot={!clear} /></div>;
    })}
    {cards.map(i => {
      const pos = positions[i.id];
      if (!pos || pos.behind || !pos.onScreen) return null;
      // A card sits on its line's middle, or near it (with a leader) when that is taken; else only its dot shows.
      const x = pos.nx * vw, y = pos.ny * vh;
      const { width: w, height: h } = routeCard(i.data, compact);
      // Where the card's centre may go: on the line's middle, then three rings around it (above, below, beside, the corners),
      // each a card further out; the first free one wins. As offsets of the card's top-centre.
      const centres: number[][] = [[0, 0]];
      for (let r = 1; r <= 3; r++) {
        const sx = r * (w / 2 + 14), sy = r * (h + 10);
        centres.push([0, -sy], [0, sy], [sx, 0], [-sx, 0], [sx, -sy / 2 - 4], [-sx, -sy / 2 - 4], [sx, sy / 2 + 4], [-sx, sy / 2 + 4]);
      }
      const spot = centres.map(([cx, cy]) => [cx, cy - h / 2]).find(([dx, dy]) => place(x + dx, y + dy + h, w, h));
      if (!spot) return <div key={i.id} className="card-anchor dot-only" style={{ left: x, top: y }}><RouteCard instance={i} dot compact={compact} /></div>;
      const [dx, dy] = spot;
      const cx = dx, cy = dy + h / 2, length = Math.hypot(cx, cy);
      return <Fragment key={i.id}>
        {length > h / 2 + 2 && <><i className="card-leader" style={{ left: x, top: y, width: length, transform: `rotate(${Math.atan2(cy, cx)}rad)` }} />
          <i className="card-dot" style={{ left: x, top: y }} /></>}
        <div className="card-anchor" style={{ left: x + dx, top: y + dy }}><RouteCard instance={i} compact={compact} /></div>
      </Fragment>;
    })}
  </div>;
}
