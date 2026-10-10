import { Term } from '@/components/Modal';
import { Sparkline } from '@/components/charts';
import { count, openCompany, percent, pounds, price } from '@/lib/format';
import { useUIStore } from '@/stores/ui-store';
import type { GrainView, MarketArchive, NeedView, TripView } from '@/types';

// 集散区的经济：集散仓的现货、需求去向与份额、远程出行、在途、工厂仓库。完整的清单在地块竞品的“集散区”一页；左侧的档案只放
// 一眼看得懂的预览（MarketPreview）。

/** A product name that opens its dossier. */
function ProductLink({ id, name }: { id: string; name: string }) {
  const openModal = useUIStore(s => s.openModal);
  return <button className="link product-link" onClick={() => openModal({ kind: 'product', subject: 'product', id })}>{name}</button>;
}

/** Where a need's demand can go, in the bar's order and colours. */
const fates = [
  { key: 'bought', label: '买到', of: (n: NeedView) => n.fulfilled },
  { key: 'nobuy', label: '不买', of: (n: NeedView) => n.noBuy },
  { key: 'supply', label: '缺货', of: (n: NeedView) => n.noSupply },
  { key: 'afford', label: '买不起', of: (n: NeedView) => n.unaffordable + n.unmaintainable },
  { key: 'access', label: '够不着', of: (n: NeedView) => n.noAccess },
];

/** Where a need's demand went last week, as the bar and the legend read it. */
const needParts = (need: NeedView) => fates.map(f => ({ key: f.key, label: f.label, value: f.of(need) })).filter(p => p.value > 0);

/** One need of a 集散区: where last week's demand went, and whose products took it. */
function NeedRow({ need }: { need: NeedView }) {
  const unmet = need.noAccess + need.unaffordable + need.unmaintainable + need.noSupply;
  const parts = needParts(need);
  return <li className="need-row">
    <div className="need-head"><b>{need.name}</b><span>{need.rigid >= 1 ? '刚需' : need.rigid > 0 ? `刚需 ${percent(need.rigid)}` : '可选'} · 需求 {count(need.demand)} 份 · 花费 {pounds(need.spend)}</span></div>
    {need.demand > 0 && <div className="need-bar" aria-label={`${need.name}需求去向`}>{parts.map(p => <i key={p.key} className={p.key} style={{ flexGrow: p.value }} title={`${p.label} ${count(p.value)}`} />)}</div>}
    <div className="need-legend">{parts.map(p => <span key={p.key} className={p.key}>{p.label} {count(p.value)}</span>)}{(need.outside ?? 0) >= 1 && <span>其中自碾 {count(need.outside ?? 0)}</span>}{need.shortage >= 1 && <span className="shortage">{need.id === 'food' ? '缺粮' : '短缺'} {count(need.shortage)}</span>}{need.postponed >= 1 && <span>预算推迟 {count(need.postponed)}</span>}{!need.demand && <span>这一周没有需求</span>}</div>
    {!!need.shares.length && <ul className="share-list">{need.shares.slice(0, 4).map(s => <li key={s.productId || s.product}>
      <span>{s.outside ? s.product : <ProductLink id={s.productId} name={s.product} />}<em className={s.mine ? 'mine' : ''}>{s.maker}</em></span><b>{percent(s.share)}</b></li>)}</ul>}
    {need.travel && <TravelShares t={need.travel} />}
    {unmet > 0 && !need.shares.length && !need.travel && <p className="quiet">区内没有卖这类商品的。</p>}
  </li>;
}

/** 远程出行：how last week's trips went (by mode) and where to; trips bumped for want of seats. */
function TravelShares({ t }: { t: NonNullable<NeedView['travel']> }) {
  if (!t.trips && !t.bumped) return null;
  return <>
    {!!t.modes.length && <ul className="share-list">{t.modes.map(m => <li key={m.mode}><span>{m.name}<em>{count(m.trips)} 人次</em></span><b>{percent(m.share)}</b></li>)}</ul>}
    {!!t.destinations.length && <p className="quiet travel-places">去向：{t.destinations.map(d => `${d.name} ${count(d.trips)}`).join('、')}{t.classes?.length ? ` · 其中${t.classes.map(c => `${c.name} ${count(c.trips)}`).join('、')}` : ''}{t.bumped > 0 ? ` · 客满没坐上 ${count(t.bumped)}` : ''}</p>}
  </>;
}

/** 远程出行：the round trips on offer tomorrow, as the households weigh them. */
function TripList({ trips }: { trips: TripView[] }) {
  if (!trips.length) return null;
  return <section className="archive-section"><h2><Term name="远程出行 · 明日">一次往返 = 去程 + 在那里停留一周 + 回程。按目的地人口与路上天数挑出几个去处，每处给出最好的走法，以及不坐某种交通工具时的走法；要去程、回程都有座才卖。驿车是模型外的公路客运，只走相邻的集散区。</Term></h2>
    <ul className="offer-list trip-list">{trips.map((t, i) => <li key={i}>
      <span><b>{t.destination}</b><em>{t.modeName}{t.className ? ` · ${t.className}` : ''} · {t.route}</em></span>
      <span className="price">{price(t.price)}<small>／往返</small></span>
      <span className="seller">{t.carrier}<em>路上 {t.days} 天</em></span>
      <span className="stock">{t.seats < 0 ? '不限座' : `余 ${count(t.seats)} 座`}</span>
    </li>)}</ul></section>;
}

/** 粮价：each grain's price at the hub over the weeks against the season's normal price, and the farmers' stores. */
function GrainPrices({ grain }: { grain: GrainView[] }) {
  if (!grain.length) return null;
  return <section className="archive-section"><h2><Term name="粮价">集散区的粮价：运到枢纽地块最便宜的农户到货价（港口还能按常年价 1.6 倍进口）。存粮比常年少，地头价就按金定律上涨；收成后最贱，青黄不接时最贵。曲线是一年里每周收盘价比这个季节常年价高低几成，横线是常年价。</Term></h2>
    <ul className="grain-list">{grain.map(g => {
      const weeks = g.weeks.slice(-52);
      // Dear or cheap against the season's normal price, as the farmers' ask is told (Agriculture.md 第 4 节).
      const tone = g.season > 0 && g.price >= g.season * 1.25 ? 'dear' : g.season > 0 && g.price <= g.season * .85 ? 'cheap' : '';
      return <li key={g.crop}>
        <b>{g.name}</b>
        <span className={`grain-price ${tone}`}>{price(g.price)}<small>／吨</small></span>
        <em>这个季节常年 {price(g.season)} · 存粮对常年同期 {percent(g.normal > 0 ? g.stores / g.normal : 0)}{g.port ? ' · 港口可进口' : ''}</em>
        {weeks.length > 1 && <Sparkline values={weeks.map(w => w.season > 0 ? w.price / w.season - 1 : 0)} width={240} height={30} label={`${g.name}一年的粮价对常年`} />}
      </li>;
    })}</ul></section>;
}

const demandNote = '每份需求的去处：买到、选择不买，或者没买到——缺货、买不起（价格超过购买预算或养不起）、够不着（没有零售、公交站或戏院）。份额按买到的份数计。';

/**
 * 集散区的全部清单（地块竞品的“集散区”一页）：左栏集散仓的现货；右栏粮价、需求去向与份额、远程出行、在途、工厂仓库。
 */
export function MarketEconomy({ archive }: { archive: MarketArchive }) {
  const offers = archive.offers ?? [];
  return <div className="modal-grid two market-page"><div className="modal-column">
    <section className="archive-section"><h2><Term name="在售">集散仓里的现货（经销商按到岸价加 18% 出售，厂商在自己的集散区按出厂价直销），以及戏院、公共马车、旅馆今天开放的座位与床位。</Term></h2>
      {offers.length ? <ul className="offer-list">{offers.map((o, i) => <li key={i}>
        <span><b><ProductLink id={o.productId} name={o.product} /></b>{o.brand && !o.product.includes(o.brand) ? <em>{o.brand}</em> : null}</span>
        <span className="price">{price(o.price)}<small>／{o.unit}</small></span>
        <span className="seller"><button className="link" onClick={() => openCompany(o.sellerId)}>{o.seller}</button><em className={o.mine ? 'mine' : ''}>{o.group}</em></span>
        <span className="stock">{o.service ? `今日 ${count(o.quantity)}` : `${count(o.quantity)} ${o.unit}`}<em>{o.service ? o.origin : `产地 ${o.origin}`}</em></span>
      </li>)}</ul> : <p className="quiet">集散仓里没有现货。</p>}</section>
  </div><div className="modal-column">
    <GrainPrices grain={archive.grain ?? []} />
    <section className="archive-section"><h2><Term name={`需求去向 · ${archive.period ?? '上周'}`}>{demandNote}</Term></h2>
      <ul className="need-list">{(archive.needs ?? []).map(n => <NeedRow key={n.id} need={n} />)}</ul></section>
    <TripList trips={archive.trips ?? []} />
    {!!archive.inbound?.length && <section className="archive-section"><h2>在途</h2><ul className="offer-list compact">{archive.inbound.slice(0, 6).map((s, i) => <li key={i}>
      <span><b>{s.product}</b></span><span className="stock">{count(s.quantity)} {s.unit}<em>{s.seller} · 自{s.from}</em></span><span className="arrive">{s.arrive} 到</span></li>)}</ul>
      {archive.inbound.length > 6 && <p className="quiet">另有 {archive.inbound.length - 6} 批在途。</p>}</section>}
    {!!archive.warehouse?.length && <section className="archive-section"><h2><Term name="工厂仓库">产出先进工厂仓库，再按缺口分给本地的直销仓和各经销商。</Term></h2><ul className="offer-list compact">{archive.warehouse.map((w, i) => <li key={i}>
      <span><b>{w.product}</b></span><span className="stock">{count(w.quantity)} {w.unit}<em>{w.maker}</em></span></li>)}</ul></section>}
  </div></div>;
}

/** One need in the preview: its name, last week's demand as one bar, and how much of it was bought. */
function NeedBar({ need }: { need: NeedView }) {
  const parts = needParts(need);
  return <li className="need-bar-row">
    <span className="need-bar-name">{need.name}{need.shortage >= 1 && <em className="shortage">{need.id === 'food' ? '缺粮' : '短缺'}</em>}</span>
    <span className="need-bar" aria-label={`${need.name}需求去向`}>{parts.map(p => <i key={p.key} className={p.key} style={{ flexGrow: p.value }} title={`${p.label} ${count(p.value)}`} />)}</span>
    <b>{percent(need.demand > 0 ? need.fulfilled / need.demand : 0)}</b>
  </li>;
}

/**
 * 集散区一览（左侧档案）：the grain's year; each need's last week as one bar (远程出行 with its modes and places); how much its depot
 * has on sale and how many trips there are tomorrow. Whose goods and trips, at what price and share, are in the 集散区 page of the
 * hub's competition.
 */
export function MarketPreview({ archive }: { archive: MarketArchive }) {
  const offers = archive.offers ?? [];
  const needs = (archive.needs ?? []).filter(n => n.demand > 0);
  const sellers = new Set(offers.map(o => o.sellerId)).size;
  const trips = archive.trips?.length ?? 0;
  return <>
    <GrainPrices grain={archive.grain ?? []} />
    <section className="archive-section"><h2><Term name={`需求去向 · ${archive.period ?? '上周'}`}>{demandNote} 各家的份额与价格、明天的车船票价在上方的“集散区的竞品与集散仓”里。</Term></h2>
      {needs.length ? <ul className="need-bars">{needs.filter(n => !n.travel).map(n => <NeedBar key={n.id} need={n} />)}</ul> : <p className="quiet">这一周没有需求。</p>}
      {!!needs.length && <div className="need-legend preview-legend">{fates.map(f => <span key={f.key} className={f.key}>{f.label}</span>)}</div>}
      <p className="quiet market-stock">集散仓在售 {count(offers.length)} 种，{count(sellers)} 家卖家{archive.inbound?.length ? `；在途 ${count(archive.inbound.length)} 批` : ''}{trips ? `；明天远程出行 ${count(trips)} 种走法` : ''}</p>
      {needs.some(n => n.travel) && <ul className="need-list">{needs.filter(n => n.travel).map(n => <NeedRow key={n.id} need={n} />)}</ul>}
    </section>
  </>;
}
