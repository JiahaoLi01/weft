import { Fragment, useMemo, useState } from 'react';
import { Modal, Cell, Tabs, Term } from '@/components/Modal';
import { HBar, LineChart, Meter, StatPairs, palette, type ChartRow, type Series } from '@/components/charts';
import { count, focusMarket, formatMeasure, percent, perUnit, pounds, price, shortPounds } from '@/lib/format';
import { useUIStore } from '@/stores/ui-store';
import { PriceForm } from './CompanyModal';
import type { CompetitorRow, ProductDossier, SubjectMarket, World } from '@/types';

type Tab = 'overview' | 'markets' | 'competition' | 'standing';
type Trend = 'revenue' | 'units' | 'price';
const productColors = [palette.gold, palette.teal, palette.violet, palette.rust, palette.green, palette.sand];

/** The subject's weekly trend: revenue or units per product, or price against unit cost. */
function Trends({ d }: { d: ProductDossier }) {
  const [trend, setTrend] = useState<Trend>('revenue');
  const single = d.products.length === 1;
  const { rows, series, format } = useMemo(() => {
    const weeks = new Map<number, ChartRow>();
    d.products.forEach((p, i) => {
      for (const w of p.history ?? []) {
        const row = weeks.get(w.week) ?? { date: w.date };
        row[`r${i}`] = w.revenue;
        row[`u${i}`] = w.units;
        row[`p${i}`] = w.price;
        if (w.unitCost !== undefined && w.unitCost > 0) row[`c${i}`] = w.unitCost;
        row.total = ((row.total as number | undefined) ?? 0) + w.revenue;
        weeks.set(w.week, row);
      }
    });
    const rows = [...weeks.entries()].sort((a, b) => a[0] - b[0]).map(e => e[1]);
    let series: Series[];
    let format = (v: number) => shortPounds(v);
    if (trend === 'revenue') series = [...d.products.map((p, i) => ({ key: `r${i}`, name: p.product, color: productColors[i % productColors.length], wide: single })),
      ...(single ? [] : [{ key: 'total', name: '合计', color: palette.ivory, dashed: true }])];
    else if (trend === 'units') { series = d.products.map((p, i) => ({ key: `u${i}`, name: `${p.product}（份）`, color: productColors[i % productColors.length], wide: single })); format = v => count(v); }
    else { series = d.products.flatMap((p, i) => [{ key: `p${i}`, name: `${p.product}售价`, color: productColors[i % productColors.length], wide: true }, { key: `c${i}`, name: `${p.product}单位成本`, color: productColors[i % productColors.length], dashed: true }]); format = v => price(v); }
    return { rows, series, format };
  }, [d.products, trend, single]);
  const tabs: { id: Trend; name: string }[] = [{ id: 'revenue', name: '收入' }, { id: 'units', name: '销量' }, { id: 'price', name: '售价与成本' }];
  return <section className="panel-block">
    <div className="block-head"><h2>每周走势</h2><div className="seg small">{tabs.map(t => <button key={t.id} className={trend === t.id ? 'active' : ''} onClick={() => setTrend(t.id)}>{t.name}</button>)}</div></div>
    <LineChart rows={rows} series={series} format={format} label={`${d.name} 每周${tabs.find(t => t.id === trend)?.name}`} empty="推进一周后显示这件产品的走势。" height={220} />
  </section>;
}

/** Attribute bars: the subject's design target against the best and the mean of its rivals of the same good. */
function AttributeCompare({ d }: { d: ProductDossier }) {
  const product = d.products[0];
  const rivals = d.competition.flatMap(b => b.rows).filter(r => !r.self && r.goodId === product?.goodId && r.attributes.length);
  if (!product?.attributes.length) return null;
  return <section className="panel-block">
    <div className="block-head"><h2><Term name="属性对比">产品的设计目标（0～100）。竖线是同一商品的对手：金色为最好的一家，灰色为平均。实际批次还受熟练度、机床精度和投入品质量影响。</Term></h2><span className="block-note">{product.product}</span></div>
    <div className="skill-list">{product.attributes.map(a => {
      const values = rivals.map(r => r.attributes.find(x => x.id === a.id)?.value).filter((v): v is number => v !== undefined);
      const best = values.length ? Math.max(...values) : undefined;
      const mean = values.length ? values.reduce((s, v) => s + v, 0) / values.length : undefined;
      return <div key={a.id} className={best !== undefined && a.value + .5 < best ? 'short' : ''}>
        <div className="skill-head"><span>{a.name}</span><b>{Math.round(a.value)}{best !== undefined && <em> / 对手最好 {Math.round(best)}</em>}</b></div>
        <div className="skill-track"><span style={{ width: `${a.value}%` }} />{mean !== undefined && <i className="mean" style={{ left: `${mean}%` }} />}{best !== undefined && <i style={{ left: `${best}%` }} />}</div>
      </div>;
    })}</div>
    {!rivals.length && <p className="quiet">在本产品有销量的集散区里还没有同一商品的对手。</p>}
  </section>;
}

function Overview({ d }: { d: ProductDossier }) {
  const markets = d.markets.filter(m => m.revenue > 0).sort((a, b) => b.revenue - a.revenue);
  const top = markets.slice(0, 6);
  const rest = markets.slice(6).reduce((s, m) => s + m.revenue, 0);
  const colors = ['#c9a96a', '#7fb3a8', '#9d8fc0', '#b98a6e', '#8796a0', '#a3b07a', '#6f6a5a'];
  return <div className="modal-grid wide-left">
    <div className="modal-column">
      <Trends d={d} />
      <section className="panel-block">
        <div className="block-head"><h2>收入来自哪里</h2><span className="block-note">上周 · {markets.length} 个集散区</span></div>
        <HBar parts={[...top.map((m, i) => ({ key: m.id, label: m.name, value: m.revenue, color: colors[i], detail: `${pounds(m.revenue)} · 份额 ${percent(Math.max(0, m.share))}` })),
          ...(rest > 0 ? [{ key: 'rest', label: '其他', value: rest, color: colors[6], detail: pounds(rest) }] : [])]} empty="上周没有收入" />
      </section>
    </div>
    <div className="modal-column">
      <div className="cells">
        <Cell label="上周销量" value={`${count(d.total.units)} 份`} />
        <Cell label="上周收入" value={shortPounds(d.total.revenue)} />
        <Cell label="同类份额" value={percent(d.total.share)} note="有销量或有货的集散区里" />
        <Cell label="在售集散区" value={count(d.total.markets)} />
        <Cell label="存货" value={count(d.total.stock)} note="集散仓与工厂仓库" />
        <Cell label="在途" value={`${d.shipments} 批`} note={d.inTransit > 0 ? `${count(d.inTransit)} 单位` : undefined} />
      </div>
      {d.products.map(p => <section key={p.productId} className="panel-block product-card">
        <div className="block-head"><h2>{p.product}</h2><span className="block-note">{p.good}{p.brand ? ` · ${p.brand}` : ''}</span></div>
        <StatPairs rows={[['标价', `${price(p.price)}／${p.unit}`], ['定价方式', p.granary ? '按粮仓的卖价' : p.marginPricing ? `按毛利率 ${percent(p.marginTarget ?? 0)}，每天随现价成本调价` : '固定价'],
          ['单位成本', p.unitCost > 0 ? `${price(p.unitCost)}（上次生产）` : '—'], ['毛利', p.unitCost > 0 ? percent(p.margin) : '—', p.margin < 0 ? 'negative' : ''],
          ...(p.currentCost !== undefined ? [['现价成本', price(p.currentCost)] as [string, string]] : []),
          ['上周', `${count(p.units)} 份 · ${pounds(p.revenue)}`], ['份额', percent(p.share)], ['设计', p.design || '默认'],
          ['产线', p.lines?.length ? p.lines.map(l => `${l.name}${l.share < 1 ? ` ${percent(l.share)}` : ''}`).join('、') : '没有产线在做'],
          ...(p.groupViews?.length ? [['各群怎么看', p.groupViews.filter(g => !g.price).map(g => `${g.name} ${g.score >= 0 ? '+' : ''}${g.score.toFixed(2)}`).join(' · ')] as [string, string]] : []),
          ...(p.ready === false ? [['状态', `工程验证中 · ${p.readyDate} 可投产`] as [string, string]] : [])]} />
        {!!p.costParts?.length && <div className="cost-parts">
          <p className="block-note">按现在的行情，每{p.unit}的成本：</p>
          <ul>{p.costParts.map((c, i) => <li key={i}><span>{c.good} {perUnit(c.perUnit)} {c.unit} × {price(c.price)}／{c.unit}<em className="detail-note">{c.source}</em></span><span>{price(c.cost)}</span></li>)}
            {p.workPerUnit !== undefined && <li><span>加工与计件<em className="detail-note">昨日</em></span><span>{price(p.workPerUnit)}</span></li>}</ul>
        </div>}
        {d.canPrice && <PriceForm productId={p.productId} current={p.price} pricing={p} />}
      </section>)}
      {d.kind === 'product' && <AttributeCompare d={d} />}
    </div>
  </div>;
}

type SortKey = 'share' | 'units' | 'revenue' | 'shelf' | 'priceIndex' | 'stock' | 'awareness' | 'loyalty' | 'needRevenue';

/** Present = sold or stocked there. Every brand starts with some awareness everywhere, so standing alone is not presence. */
const present = (m: SubjectMarket) => m.revenue > 0 || m.stock > 0;

function Markets({ d, highlight, onClose }: { d: ProductDossier; highlight?: string; onClose: () => void }) {
  const [sort, setSort] = useState<SortKey>('revenue');
  const [all, setAll] = useState(false);
  const shown = useMemo(() => {
    const rows = all ? d.markets.filter(m => present(m) || m.needRevenue > 0) : d.markets.filter(present);
    return [...rows].sort((a, b) => ((b[sort] ?? -1) as number) - ((a[sort] ?? -1) as number) || a.name.localeCompare(b.name)).slice(0, all ? 80 : 200);
  }, [d.markets, sort, all]);
  const absent = d.markets.filter(m => !present(m) && m.needRevenue > 0).length;
  const head = (key: SortKey, name: string) => <span className={`th num sortable ${sort === key ? 'sorted' : ''}`} onClick={() => setSort(key)}>{sort === key ? '▾ ' : ''}{name}</span>;
  return <section className="panel-block">
    <div className="block-head"><h2><Term name="各集散区">上周各集散区的表现。份额 = 本对象收入 ÷ 它所在需求（企业采购按商品）的全部收入；货架价 = 集散仓货架上最低的一批（经销商按到岸价加价，厂商在本地按出厂价），家庭付的到户价还要加送货费；比价 = 每份需求的收入对其他卖家均价的比（100% 为持平，低于 100% 更便宜）；知名度与忠诚度是品牌在该集散区的声望。点列名排序。</Term></h2>
      <span className="block-note">{absent > 0 && <button className="link" onClick={() => { setAll(!all); setSort(all ? 'revenue' : 'needRevenue'); }}>{all ? '只看有销量或有货的' : `另有 ${absent} 个集散区买同类商品而没有本对象`}</button>}</span></div>
    <div className="table market-table">
      <span className="th">集散区</span>{head('share', '份额')}{head('units', '销量')}{head('revenue', '收入')}{head('shelf', '货架价')}{head('priceIndex', '比价')}{head('stock', '库存')}{head('awareness', '知名度')}{head('loyalty', '忠诚度')}<span className="th">主要对手</span>
      {shown.map(m => <Fragment key={m.id}>
        <span className={`market-name ${m.id === highlight ? 'highlight' : ''}`}><button className="link" onClick={() => { focusMarket(m.lon, m.lat); onClose(); }}>{m.name}</button><em>{m.country}</em></span>
        <span className="figure-cell">{formatMeasure('share', m.share)}{m.share >= 0 && <i className="cell-bar" style={{ width: `${Math.min(100, m.share * 100)}%` }} />}</span>
        <span className="figure-cell">{m.units > 0 ? count(m.units) : '—'}</span>
        <span className="figure-cell">{m.revenue > 0 ? pounds(m.revenue) : all ? <em>需求 {shortPounds(m.needRevenue)}</em> : '—'}</span>
        <span className="figure-cell">{m.shelf !== undefined && m.shelf >= 0 ? price(m.shelf) : '—'}</span>
        <span className={`figure-cell ${m.priceIndex > 1.1 ? 'dear' : m.priceIndex >= 0 && m.priceIndex < .9 ? 'cheap' : ''}`}>{formatMeasure('price', m.priceIndex)}</span>
        <span className="figure-cell">{m.stock > 0 ? count(m.stock) : '—'}</span>
        <span className="figure-cell">{formatMeasure('awareness', m.awareness)}</span>
        <span className="figure-cell">{formatMeasure('loyalty', m.loyalty)}</span>
        <span className="rivals">{m.rivals.slice(0, 2).map(r => `${r.name} ${percent(r.share)}`).join('、') || '—'}</span>
      </Fragment>)}
    </div>
    {all && <p className="quiet">按需求的规模列出前 80 个；知名度与忠诚度是品牌在当地的声望，开局时外国也有一点。</p>}
    {!shown.length && <p className="quiet">还没有在任何集散区卖出或存货。</p>}
  </section>;
}

function Competition({ d }: { d: ProductDossier }) {
  const openModal = useUIStore(s => s.openModal);
  const subjectGood = d.products[0]?.goodId;
  const attributeNames = d.products[0]?.attributes ?? [];
  if (!d.competition.length) return <p className="quiet">本对象所在的需求上周没有成交。</p>;
  const colors = ['#c9a96a', '#7fb3a8', '#9d8fc0', '#b98a6e', '#8796a0', '#a3b07a', '#6f8f9a', '#9b6f62'];
  return <div className="competition">{d.competition.map(block => <section key={block.key} className="panel-block">
    <div className="block-head"><h2><Term name={block.name}>只统计本对象有销量或有货的集散区里、同一需求（企业采购按商品）的成交。金色为本对象。</Term></h2><span className="block-note">上周 {pounds(block.revenue)}</span></div>
    <HBar parts={block.rows.map((r, i) => ({ key: r.productId || r.product, label: r.product, value: r.revenue, color: r.self ? '#d9b46c' : colors[(i + 1) % colors.length], detail: `${r.maker} · ${percent(r.share)}` }))} />
    <div className="table competitor-table" style={{ gridTemplateColumns: `minmax(0, 1.4fr) minmax(0, 1fr) repeat(${4 + (attributeNames.length ? attributeNames.length : 0)}, minmax(0, .7fr))` }}>
      <span className="th">产品</span><span className="th">厂商</span><span className="th num">份额</span><span className="th num">销量</span><span className="th num">均价／份</span><span className="th num">标价</span>
      {attributeNames.map(a => <span key={a.id} className="th num">{a.name}</span>)}
      {block.rows.map((r: CompetitorRow) => <Fragment key={r.productId || r.product}>
        <span className={r.self ? 'self' : ''}>{r.outside ? r.product : <button className="link" onClick={() => openModal({ kind: 'product', subject: 'product', id: r.productId })}>{r.product}</button>}</span>
        <span className={r.mine ? 'mine' : ''}>{r.maker}<em>{r.group}</em></span>
        <span className="figure-cell">{percent(r.share)}</span>
        <span className="figure-cell">{count(r.units)}</span>
        <span className="figure-cell">{r.averagePrice > 0 ? price(r.averagePrice) : '—'}</span>
        <span className="figure-cell">{r.listPrice >= 0 ? price(r.listPrice) : '—'}</span>
        {attributeNames.map(a => <span key={a.id} className="figure-cell">{r.goodId === subjectGood ? Math.round(r.attributes.find(x => x.id === a.id)?.value ?? 50) : '—'}</span>)}
      </Fragment>)}
    </div>
  </section>)}</div>;
}

type Tier = { awareness: number; loyalty: number; markets: SubjectMarket[] };
const tiersShown = 12;

function StandingRow({ m }: { m: SubjectMarket }) {
  return <div className="standing-row">
    <span className="standing-name">{m.name}<em>{m.country}</em></span>
    <Meter label="知名度" value={m.awareness} text={percent(m.awareness)} />
    <Meter label="忠诚度" value={Math.max(0, m.loyalty)} text={formatMeasure('loyalty', m.loyalty)} tone="loyal" />
  </div>;
}

/** Brand standing: in full where the subject sells or stocks; elsewhere grouped by equal standing (mostly the opening tiers). */
function Standing({ d }: { d: ProductDossier }) {
  const [each, setEach] = useState(false);
  if (!d.brandId) return <p className="quiet">同质商品没有品牌：买家只看价格和产地，没有知名度与忠诚度。</p>;
  const rows = d.markets.filter(m => m.awareness >= 0).sort((a, b) => b.awareness - a.awareness || b.loyalty - a.loyalty || a.name.localeCompare(b.name));
  const active = rows.filter(present);
  const rest = rows.filter(m => !present(m));
  const tiers = new Map<string, Tier>();
  for (const m of rest) {
    const key = `${Math.round(m.awareness * 100)}|${Math.round(Math.max(0, m.loyalty) * 100)}`;
    const tier = tiers.get(key) ?? { awareness: m.awareness, loyalty: Math.max(0, m.loyalty), markets: [] };
    tier.markets.push(m);
    tiers.set(key, tier);
  }
  const groups = [...tiers.values()].sort((a, b) => b.awareness - a.awareness || b.loyalty - a.loyalty);
  return <section className="panel-block">
    <div className="block-head"><h2><Term name="品牌声望">知名度随销量与曝光增长；忠诚度随信任变化，重组时降两成。两者都按集散区记；开局时品牌在本地最响，同国次之，外国也有一点。</Term></h2><span className="block-note">{d.brand} · {rows.length} 个集散区</span></div>
    {!rows.length && <p className="quiet">这个品牌还没有在任何集散区建立声望。</p>}
    {active.length > 0 && <><h3>有销量或有货的集散区</h3><div className="standing-list">{active.map(m => <StandingRow key={m.id} m={m} />)}</div></>}
    {rest.length > 0 && <>
      <div className="block-head standing-rest-head"><h3>其他集散区 · {rest.length} 个</h3><button className="link" onClick={() => setEach(!each)}>{each ? '按声望分组' : '逐个列出'}</button></div>
      {each ? <div className="standing-list">{rest.map(m => <StandingRow key={m.id} m={m} />)}</div>
        : <ul className="standing-tiers">{groups.slice(0, tiersShown).map(t => <li key={`${t.awareness}|${t.loyalty}`}>
          <span className="tier-figures">知名度 <b>{percent(t.awareness)}</b> · 忠诚度 <b>{percent(t.loyalty)}</b></span>
          <span className="tier-markets">{t.markets.length} 个：{t.markets.slice(0, 6).map(m => m.name).join('、')}{t.markets.length > 6 ? ' 等' : ''}</span>
        </li>)}{groups.length > tiersShown && <li className="tier-more">另有 {groups.length - tiersShown} 档，逐个列出可看全部。</li>}</ul>}
    </>}
  </section>;
}

export function ProductModal({ world, subject, id, market, tab: initial, onClose }: { world: World; subject: 'product' | 'brand'; id: string; market?: string; tab?: string; onClose: () => void }) {
  const [tab, setTab] = useState<Tab>((initial as Tab) ?? (market ? 'markets' : 'overview'));
  const openModal = useUIStore(s => s.openModal);
  const d = world.productDossier?.kind === subject && world.productDossier.id === id ? world.productDossier : undefined;
  if (!d) return <Modal eyebrow={subject === 'brand' ? '品牌' : '产品'} title="正在读取档案…" onClose={onClose}><p className="quiet">正在读取档案…</p></Modal>;
  const first = d.products[0];
  const tabs: { id: Tab; name: string; badge?: number }[] = [{ id: 'overview', name: '概览' }, { id: 'markets', name: '各集散区', badge: d.total.markets },
    { id: 'competition', name: '竞争' }, { id: 'standing', name: '品牌声望' }];
  return <Modal className="product-modal" onClose={onClose}
    eyebrow={<>{d.kind === 'brand' ? `品牌 · ${d.products.length} 种产品` : `产品 · ${first?.good ?? ''}`}{d.kind === 'product' && d.brand ? ` · 品牌 ${d.brand}` : ''}</>}
    title={d.name}
    meta={<><button className="link" onClick={() => openModal({ kind: 'company', id: d.companyId, tab: 'products' })}>{d.company}</button><em className={d.mine ? 'mine' : ''}>{d.group || '无控制集团'}</em>
      {d.kind === 'product' && d.brandId && <button className="link" onClick={() => openModal({ kind: 'product', subject: 'brand', id: d.brandId! })}>品牌档案</button>}
      {!!d.siblings?.length && <span>同品牌：{d.siblings.map((s, i) => <Fragment key={s.id}>{i > 0 && '、'}<button className="link" onClick={() => openModal({ kind: 'product', subject: 'product', id: s.id })}>{s.name}</button></Fragment>)}</span>}</>}
    figures={<><Cell label="上周收入" value={shortPounds(d.total.revenue)} /><Cell label="同类份额" value={percent(d.total.share)} />{first && d.kind === 'product' && <Cell label="标价" value={price(first.price)} note={first.unitCost > 0 ? `毛利 ${percent(first.margin)}` : undefined} />}</>}
    tabs={<Tabs tabs={tabs} value={tab} onChange={setTab} />}>
    {tab === 'overview' && <Overview d={d} />}
    {tab === 'markets' && <Markets d={d} highlight={market} onClose={onClose} />}
    {tab === 'competition' && <Competition d={d} />}
    {tab === 'standing' && <Standing d={d} />}
  </Modal>;
}
