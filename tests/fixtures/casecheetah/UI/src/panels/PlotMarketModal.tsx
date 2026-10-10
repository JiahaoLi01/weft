import { Fragment, useMemo, useState } from 'react';
import { Cell, Modal, Tabs, Term } from '@/components/Modal';
import { Hoverable } from '@/components/tooltip/TooltipSystem';
import { MarketEconomy } from './MarketEconomy';
import { count, decimal, people, percent, price, setShareFocus } from '@/lib/format';
import { useUIStore } from '@/stores/ui-store';
import type { PlotNeed, PlotOffer, World } from '@/types';

// 地块竞品：这个地块的家庭看到的每一份报价——到户价、质量、声望，以及三档收入各自的预计选择。

type SortKey = 'choice' | 'delivered' | 'shelf' | 'score' | 'week';
const blocked = ['够不着', '买不起', '养不起', ''];

/**
 * 每档的预计选择，下面一行是“愿付”：这一档里不只看价钱的买家为它最多肯付的到户价（各群按人数平均）；到户价在它以内是金色。悬停看
 * 各群：只看价钱的只认最便宜的，看品级、看洁净、求均衡的各愿付多少。
 */
function TierShares({ o, tiers }: { o: PlotOffer; tiers: string[] }) {
  return <span className="tier-shares">{o.shares.map((s, t) => {
    const w = o.wtp?.[t];
    const cell = <span className="tier-cell">
      <span className={`tier-share ${o.progress[t] < 3 ? 'blocked' : ''}`}>
        <i style={{ width: `${Math.min(100, s * 100)}%` }} /><b>{o.progress[t] < 3 ? blocked[o.progress[t]] : percent(s)}</b></span>
      {w?.price !== undefined && <em className={`tier-wtp ${o.delivered <= w.price ? 'within' : ''}`}>{price(w.price)}</em>}
    </span>;
    if (!w?.groups.length) return <Fragment key={t}>{cell}</Fragment>;
    return <Hoverable key={t} pinnable={false} width={270} content={<div className="marker-tip">
      <b>{tiers[t] ?? ''} · 到户价 {price(o.delivered)}</b>
      {w.groups.map(g => <span key={g.name}>{g.name} {percent(g.share)}：{g.quality ? `最多愿付 ${price(g.price)}` : `只认最便宜的（${price(g.price)}）`}</span>)}
    </div>}>{cell}</Hoverable>;
  })}</span>;
}

/** The buyers' groups in order (热价比, 看一项, 看另一项, 求均衡…): cost ochre, gold, teal, lilac, then the quieter ones. */
const groupColors = ['#b98a6e', '#c9a96a', '#7fb3a8', '#9d8fc0', '#8796a0', '#a3b07a'];

/**
 * 各收入组（Demand.md 第 5 节）：what each income group wants a day and, for a need with buyer groups, its affordability a and its
 * buyers by group as one bar — the groups' colours named once above, the exact shares on hover.
 */
function TierGroups({ need, tiers }: { need: PlotNeed; tiers: string[] }) {
  const names = need.groupNames ?? [];
  const grouped = !!names.length && !!need.tierGroups?.length;
  return <div className={`tier-groups ${grouped ? 'grouped' : ''}`}>
    {grouped && <div className="tier-groups-head">
      <Term name="人群">每项需求的买家分成几群，各看各的：有的只看价钱（热价比），有的看某一项，有的求均衡（几项的乘积）。各群占多少、看质量的人对价钱有多敏感，都取决于可负担度 a：一天的收入够付几天的这项需求加上最低限度的其他开销（房租、柴火、衣服，按当地物价）。越穷、东西越贵，只看价钱的人越多，看质量的人也越舍不得多花钱。表里每档下面的“愿付”，是这一档里看质量的人为这份报价最多肯付的到户价；到户价在它以内显示成金色。</Term>
      <span className="group-legend">{names.map((n, k) => <span key={n}><i style={{ backgroundColor: groupColors[k % groupColors.length] }} />{n}</span>)}</span>
    </div>}
    {tiers.map((t, i) => {
      const g = need.tierGroups?.[i];
      return <div key={t} className="tier-group-row">
        <b>{t}</b>
        <span className="tier-wants">每天想买 <em>{count(need.wants[i] ?? 0)}</em> {need.unit}{(need.noBuy[i] ?? 0) > 0.0005 ? ` · 不买 ${percent(need.noBuy[i])}` : ''}{(need.unreached[i] ?? 0) > 0.0005 ? ` · 买不到 ${percent(need.unreached[i])}` : ''}</span>
        {grouped && g && <>
          <span className="tier-a">a <em>{g.a.toFixed(1)}</em></span>
          <Hoverable className="group-bar-hit" pinnable={false} width={250} content={<div className="marker-tip"><b>{t} · a = {g.a.toFixed(1)}</b>
            {names.map((n, k) => <span key={n}>{n} {percent(g.shares[k] ?? 0)}</span>)}</div>}>
            <span className="group-bar" aria-label={`${t}：${names.map((n, k) => `${n} ${percent(g.shares[k] ?? 0)}`).join('、')}`}>
              {names.map((n, k) => (g.shares[k] ?? 0) > 0.0005 && <i key={n} style={{ flexGrow: g.shares[k], backgroundColor: groupColors[k % groupColors.length] }}>{g.shares[k] >= .1 ? percent(g.shares[k]) : ''}</i>)}
            </span>
          </Hoverable>
        </>}
      </div>;
    })}
  </div>;
}

function NeedTable({ need, tiers, onClose }: { need: PlotNeed; tiers: string[]; onClose: () => void }) {
  const openModal = useUIStore(s => s.openModal);
  const [sort, setSort] = useState<SortKey>('choice');
  const middle = Math.min(1, tiers.length - 1);
  const rows = useMemo(() => [...need.offers].sort((a, b) =>
    sort === 'delivered' ? a.delivered - b.delivered : sort === 'shelf' ? a.shelf - b.shelf : sort === 'score' ? b.score - a.score
      : sort === 'week' ? b.weekUnits - a.weekUnits : (b.shares[middle] ?? 0) - (a.shares[middle] ?? 0) || a.delivered - b.delivered), [need.offers, sort, middle]);
  const goods = useMemo(() => {
    const seen = new Map<string, string>();
    for (const o of need.offers) if (!seen.has(o.goodId)) seen.set(o.goodId, o.good);
    return [...seen.entries()];
  }, [need.offers]);
  const head = (key: SortKey, name: string) => <span className={`th num sortable ${sort === key ? 'sorted' : ''}`} onClick={() => setSort(key)}>{sort === key ? '▾ ' : ''}{name}</span>;
  return <section className="panel-block">
    <div className="block-head"><h2><Term name={need.name}>{need.essential ? '必需需求：没有“不买”这个选项。' : '可选需求：嫌贵或看不上时可以不买。'}家庭先在品牌之间比（同质商品按厂商），同一品牌的几款再分。到户价 = 货架价 + 从集散仓送到这个地块的送货费；送货费按货物重量、本地运价与这个地块的道路阻力计。向农户、港口买粮自碾的，货架价是每吨的地头价或进口价，到户价按一份折算，另含自家舂碾的工夫。</Term></h2>
      <span className="block-note">集散区上周成交 {count(need.weekUnits)} {need.unit}</span></div>
    <TierGroups need={need} tiers={tiers} />
    <div className="table plot-offers">
      <span className="th">产品</span><span className="th">卖家 · 产地</span>{head('shelf', '货架价')}<span className="th num">送货</span>{head('delivered', '到户价／份')}
      <span className={`th sortable ${sort === 'score' ? 'sorted' : ''}`} onClick={() => setSort('score')}>{sort === 'score' ? '▾ ' : ''}质量与声望</span>
      <span className="th num">存货</span>{head('week', '上周份额')}{head('choice', `预计选择 · 愿付（${tiers.map(t => t.slice(0, 1)).join(' · ')}）`)}
      {rows.map(o => <Fragment key={`${o.productId}|${o.sellerId}`}>
        <span className={`offer-name ${o.mine ? 'mine' : ''}`}>{o.outside ? <b>{o.product}</b> : <button className="link" onClick={() => openModal({ kind: 'product', subject: 'product', id: o.productId })}>{o.product}</button>}
          {o.main && <i className="offer-tag">主力</i>}{o.selected && <i className="offer-tag picked">选中</i>}
          <em>{o.brand || '无品牌'} · {o.maker}{o.group ? ` · ${o.group}` : ''}</em></span>
        <span className="offer-seller">{o.seller === o.maker ? '厂商直营' : o.seller}<em>{o.service ? '本地服务' : o.origin ? `产地 ${o.origin}` : ''}</em></span>
        <span className="figure-cell">{price(o.shelf)}<em>／{o.unit}</em></span>
        <span className="figure-cell">{o.fee > 0 ? price(o.fee) : '—'}</span>
        <span className="figure-cell delivered">{price(o.delivered)}{o.useCost > 0 && <em>年养护 {price(o.useCost)}</em>}{(o.work ?? 0) > 0 && <em>含自家舂碾 {price(o.work ?? 0)}</em>}</span>
        <span className="offer-quality">{o.attributes.map(a => `${a.name} ${a.value}`).join(' · ') || '—'}
          <em>{[o.awareness >= 0 ? `知名度 ${percent(o.awareness)} · 忠诚度 ${percent(o.loyalty)}` : '',
            // 分群的需求，属性由各群去看，吸引力里只有中性的属性（Demand.md 第 5.2 节）：不显示。
            need.groupNames?.length ? '' : `吸引力 ${o.score.toFixed(2)}`].filter(Boolean).join(' · ')}</em></span>
        <span className="figure-cell">{o.stock >= 0 ? count(o.stock) : '不限'}<em>{o.stock >= 0 ? o.unit : ''}</em></span>
        <span className="figure-cell">{o.weekShare >= 0 ? percent(o.weekShare) : '—'}<em>{count(o.weekUnits)} {need.unit}</em></span>
        <TierShares o={o} tiers={tiers} />
      </Fragment>)}
    </div>
    <div className="need-goods">{goods.map(([id, name]) => <button key={id} className="link" onClick={() => { setShareFocus('good', id, 'main'); onClose(); }}>地图：{name}各地块的主力商品价格与运价</button>)}</div>
  </section>;
}

export function PlotMarketModal({ world, plot, need, onClose }: { world: World; plot: string; need?: string; onClose: () => void }) {
  const m = world.plotMarket?.plot === plot ? world.plotMarket : undefined;
  const [needId, setNeedId] = useState(need ?? '');
  if (!m) return <Modal eyebrow="地块竞品" title="正在读取…" onClose={onClose}><p className="quiet">正在读取地块的报价…</p></Modal>;
  const needs = m.needs ?? [];
  // 集散区：the 集散区's own lists, a page after the needs (opened at once from a 集散区's archive).
  const archive = m.marketArchive;
  const onMarket = needId === 'market' && !!archive;
  const current = needs.find(n => n.id === needId) ?? needs.find(n => n.offers.some(o => o.mine)) ?? needs[0];
  const tiers = (m.tiers ?? []).map(t => t.name);
  const pages = [...needs.map(n => ({ id: n.id, name: n.name, badge: n.offers.length })), ...(archive ? [{ id: 'market', name: '集散区', badge: archive.offers?.length ?? 0 }] : [])];
  return <Modal className="plot-market-modal" onClose={onClose}
    eyebrow={<>地块竞品 · {m.marketName}{m.country ? ` · ${m.country}` : ''}</>}
    title={m.name}
    meta={m.reason ? undefined : <>
      <span>人口 {people(m.population ?? 0)}</span>
      <span><Term name={`道路阻力 ${(m.road ?? 1).toFixed(2)}`}>从集散仓把货送到这个地块的难易：送货费按它放大。1 是枢纽附近的平路。</Term></span>
      <span>本地运价 ×{(m.localRate ?? 1).toFixed(2)}</span>
      <span><Term name={`送货费率 ×${(m.deliveryFare ?? 1).toFixed(2)}`}>{m.stations?.length
        ? `这个地块的马车站按运价从低到高承运，运不完的由模型外车夫按基准价运；家庭明天看到的送货费 = 基准 × 道路阻力 × 今天的平均费率。${m.stations.map(s => `${s.company}${s.group ? `（${s.group}）` : ''}：每天 ${decimal(s.tonsPerDay)} 吨，运价 ×${s.fare.toFixed(2)}`).join('；')}。`
        : '这个地块没有马车站：全由模型外车夫按基准价送货。建一座马车站能提高零售覆盖、降低本地块的送货费。'}</Term></span>
      {(m.freightCoverage ?? 0) > 0 && <span>配送覆盖 {percent(m.freightCoverage ?? 0)}</span>}
      {(m.coverage ?? []).map(c => <span key={c.name}>{c.name}覆盖 {percent(c.share)}</span>)}</>}
    figures={<>{(m.tiers ?? []).map(t => <Cell key={t.tier} label={t.name} value={percent(t.share)} note={`收入 ×${t.income.toFixed(2)}`} />)}</>}
    tabs={pages.length ? <Tabs tabs={pages} value={onMarket ? 'market' : current?.id ?? ''} onChange={setNeedId} /> : undefined}>
    {onMarket ? <MarketEconomy archive={archive!} />
      : m.reason ? <p className="quiet">{m.reason}</p> : current ? <NeedTable key={current.id} need={current} tiers={tiers} onClose={onClose} /> : <p className="quiet">这个集散区今天没有任何报价。</p>}
  </Modal>;
}
