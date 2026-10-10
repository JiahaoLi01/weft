import { Fragment, useEffect, useMemo, useState } from 'react';
import { BrandsPanel, ProductControls, ProductDesigner, ProductStages } from './ProductOps';
import { Modal, Cell, StatusBadge, Tabs, Term, MoneyInput } from '@/components/Modal';
import { Select } from '@/components/Select';
import { HBar, LineChart, Sparkline, UtilBars, palette, type BarPart, type ChartRow } from '@/components/charts';
import { command, count, facilityGlyph, focusFacility, percent, pounds, price, shortPounds, signedPounds, statusTone } from '@/lib/format';
import { focusOn } from '@/lib/focus';
import { useUIStore } from '@/stores/ui-store';
import { FinancePanel, KnowledgePanel, ProductDesign, ProjectRow } from './Step4';
import { TransportPanel } from './TransportPanel';
import { MiningPanel } from './Mining';
import { RailPanel } from './RailPanel';
import { LineBriefs } from './FacilityModal';
import type { Company, Flow, Injector, Pricing, World } from '@/types';

type Tab = 'overview' | 'works' | 'transport' | 'rail' | 'mining' | 'products' | 'projects' | 'knowledge' | 'finance' | 'accounts';
const holderColors = ['#c9a96a', '#7fb3a8', '#9d8fc0', '#b98a6e', '#8796a0', '#a3b07a'];

/** 分红：按持股比例发给所有股东；散户的部分流出模型。 */
export function DividendForm({ company, cash }: { company: string; cash: number }) {
  const [amount, setAmount] = useState('');
  const presets = [.1, .25, .5].map(s => Math.floor(cash * s / 10) * 10).filter(v => v >= 10);
  return <div className="inline-form">
    <MoneyInput label="分红" value={amount} onChange={setAmount} />
    {presets.map(p => <button key={p} className="link" onClick={() => setAmount(String(p))}>{shortPounds(p)}</button>)}
    <button className="action" disabled={!Number(amount) || Number(amount) > cash} onClick={() => { command('corp.pay_dividend', company, { amount }); setAmount(''); }}>按持股比例发放</button>
  </div>;
}

/** 注资：按每股净资产认购新股。全资时只是出资；有别的股东时是增发，要 75% 表决权。 */
export function InjectForm({ company, injectors, sharePrice, canMajor }: { company: string; injectors: Injector[]; sharePrice: number; canMajor: boolean }) {
  const [from, setFrom] = useState(injectors[0]?.id ?? '');
  const [amount, setAmount] = useState('');
  const investor = injectors.find(i => i.id === from);
  if (!injectors.length) return <p className="quiet">你的集团没有能出资的信托或控股公司。</p>;
  const allowed = !!investor && (investor.wholly || canMajor);
  return <div className="inject">
    <div className="inline-form">
      <span>由</span><Select value={from} onChange={setFrom} ariaLabel="出资方" options={injectors.map(i => ({ value: i.id, label: `${i.name}（${shortPounds(i.cash)}）` }))} />
      <MoneyInput label="注资" value={amount} onChange={setAmount} />
      <button className="action" disabled={!allowed || !Number(amount) || Number(amount) > (investor?.cash ?? 0)} onClick={() => { command('corp.inject_capital', company, { from, amount }); setAmount(''); }}>认购新股</button>
    </div>
    <p className="quiet">{investor?.wholly ? '全资：出资换成新股，持股比例不变。' : canMajor
      ? `增发：按每股净资产 ${price(sharePrice)} 认购，其他股东被稀释（重大事项，已有 75% 表决权）。`
      : '增发会稀释其他股东，属于重大事项：需要 75% 的表决权。'}{Number(amount) > 0 ? ` 约 ${count(Number(amount) / Math.max(.01, sharePrice))} 股。` : ''}</p>
  </div>;
}

/** 改名：经营这家公司的集团可以改它的名字（身份是 ID，名字是运行数据）。 */
function Rename({ c }: { c: Company }) {
  const [open, setOpen] = useState(false);
  const [name, setName] = useState(c.name);
  if (!c.canOperate) return null;
  if (!open) return <button className="link" onClick={() => { setName(c.name); setOpen(true); }}>改名</button>;
  return <span className="inline-form rename">
    <input value={name} onChange={e => setName(e.target.value)} style={{ width: 160 }} aria-label="新名称" />
    <button className="link" disabled={!name.trim() || name.trim() === c.name} onClick={() => { command('corp.rename', c.id, { name: name.trim() }); setOpen(false); }}>确定</button>
    <button className="link" onClick={() => setOpen(false)}>取消</button>
  </span>;
}

function Overview({ c }: { c: Company }) {
  const history = c.history ?? [];
  const rows: ChartRow[] = history.map(w => ({ date: w.date, cash: w.cash, operating: w.operating, income: w.income }));
  const last = history[history.length - 1];
  const holders: BarPart[] = c.shareholders.map((h, i) => ({ key: h.holder + i, label: h.holder, value: h.share, color: holderColors[i % holderColors.length], detail: `${percent(h.share)}${h.votes ? ' · 投票' : ' · 不投票'}${h.group && h.group !== h.holder ? ` · ${h.group}` : ''}` }));
  return <div className="modal-grid wide-left">
    <div className="modal-column">
      <section className="panel-block">
        <div className="block-head"><h2><Term name="经营走势">每周收盘：现金余额、经营收入、经营利润（经营收入 − 经营开支；借款、还款、资本金、分红与投资另计）。</Term></h2></div>
        <LineChart rows={rows} series={[{ key: 'cash', name: '现金', color: palette.gold, wide: true }, { key: 'income', name: '经营收入', color: palette.ivory, dashed: true }, { key: 'operating', name: '经营利润', color: palette.teal }]}
          format={shortPounds} label={`${c.name} 每周现金与经营利润`} height={230} />
      </section>
      <div className="cells">
        <Cell label="上周经营利润" value={last ? signedPounds(last.operating) : '—'} tone={last && last.operating < 0 ? 'negative' : 'positive'} note={c.finance?.credit ? `平滑 ${signedPounds(c.finance.credit.profitAvg)}` : undefined} />
        <Cell label="上周投资" value={last ? pounds(last.investment) : '—'} note="施工、设备、研发" />
        <Cell label="困境" value={c.finance?.credit?.distress ? `${c.finance.credit.distress} / ${c.finance.credit.distressLimit} 周` : '无'} tone={c.finance?.credit?.distress ? 'negative' : ''} />
        <Cell label="在研" value={c.knowledge?.research?.name ?? '—'} note={c.knowledge?.research ? percent(c.knowledge.research.progress) : undefined} />
      </div>
    </div>
    <div className="modal-column">
      <section className="panel-block">
        <div className="block-head"><h2><Term name="股东">按参与投票的股份计算控制：最大持股人经营，75% 以上可做重大事项，25% 以上可否决。散户和银行手里的抵押股不投票。</Term></h2><span className="block-note">{count(c.placed ?? 0)} 股 · 每股净资产 {price(c.sharePrice ?? 0)}</span></div>
        <HBar parts={holders} />
        <ul className="holder-list">{c.shareholders.map((h, i) => <li key={i}><span>{h.holder}{h.group && h.group !== h.holder ? <em> · {h.group}</em> : null}</span><b>{percent(h.share)}</b><i>{h.votes ? '投票' : '不投票'}</i></li>)}</ul>
      </section>
      {(c.canDividend || !!c.injectors?.length) && <section className="panel-block">
        <div className="block-head"><h2><Term name="资本">两个顶层法人之间不能直接挪钱：钱向上靠分红（按持股比例，经营事项），向下靠注资（认购新股；有其他股东时是增发，重大事项）。</Term></h2></div>
        {c.canDividend && <DividendForm company={c.id} cash={c.cash} />}
        {!!c.injectors?.length && <InjectForm company={c.id} injectors={c.injectors} sharePrice={c.sharePrice ?? 0} canMajor={c.canMajor} />}
      </section>}
    </div>
  </div>;
}

function Works({ c, onClose }: { c: Company; onClose: () => void }) {
  const openModal = useUIStore(s => s.openModal);
  const setSiting = useUIStore(s => s.setSiting);
  return <div>
    <div className="card-grid">{c.facilities.map(f => {
      const line = f.production;
      const ratio = f.ratio !== undefined && f.ratio >= 0 ? f.ratio : line?.goods[0] && line.goods[0].nominal > 0 ? line.goods[0].produced / line.goods[0].nominal : 0;
      return <article key={f.id} className={`works-card ${statusTone(f.status)}`}>
        <div className="works-head"><i className="glyph">{facilityGlyph[f.typeId ?? ''] ?? f.type.slice(0, 1)}</i>
          <button className="link name" onClick={() => openModal({ kind: 'facility', id: f.id })}>{f.name}</button>{f.status && <StatusBadge status={f.status} tone={statusTone(f.status)} />}</div>
        <span className="works-meta">{f.type}{f.lines?.length ? '' : f.size > 1 ? ` · 规模 ${f.size}` : ''} · {f.plot} · {f.market}</span>
        {line ? <>
          <div className="works-figures"><span>上周利用率 <b>{percent(ratio)}</b></span>{!!f.spark?.length && <Sparkline values={f.spark.map(v => v * 100)} label="每周利用率" />}</div>
          <UtilBars ratio={ratio} />
          {!!f.lines?.length && <LineBriefs lines={f.lines} />}
          <span className="works-meta">{f.lines?.length ?? 1} 条产线 · 班组 {count(line.heads)}/{count(line.required)} · 熟练度 {percent(line.effective)}{line.limit ? ` · 受限：${line.limit}` : ''}</span>
        </> : <span className="works-meta">{f.project ? `工程：${f.project.name}` : '服务或经销设施'}</span>}
        <div className="works-actions"><button className="link" onClick={() => openModal({ kind: 'facility', id: f.id })}>{line ? '打开产线' : '打开设施'}</button>
          <button className="link" onClick={() => { focusFacility(f.id); onClose(); }}>在地图上定位</button></div>
      </article>;
    })}</div>
    {!c.facilities.length && <p className="quiet">名下没有设施。</p>}
    {c.canOperate && c.kind === '经营公司' && <div className="siting-call">
      <button className="action" onClick={() => { setSiting({ company: c.id, name: c.name }); onClose(); }}>新建设施：在地图上选址</button>
      <p className="quiet">只能建{c.business || '本业务'}的设施；别的行业要在集团里另开姊妹公司。</p>
    </div>}
  </div>;
}

/** 定价，两种方案：固定价——价格不动，直到有人改它；按毛利率——每天按现价成本调价，使（价 − 成本）÷ 价 = 目标毛利率。
 *  毛利率可以是负的（倾销）。手动定一个价就回到固定价。 */
export function PriceForm({ productId, current, pricing }: { productId: string; current: number; pricing?: Pricing }) {
  const byMargin = !!pricing?.marginPricing;
  const cost = pricing?.currentCost ?? 0;
  // The margin offered when switching: the one set before, else today's margin over the current cost, else 15%.
  const suggested = byMargin || pricing?.marginTarget ? pricing?.marginTarget ?? .15 : cost > 0 && current > cost ? (current - cost) / current : .15;
  const [value, setValue] = useState('');
  const [choosing, setChoosing] = useState(false);
  const [margin, setMargin] = useState('');
  const shown = margin !== '' ? margin : (suggested * 100).toFixed(1).replace(/\.0$/, '');
  const m = Number(shown);
  const valid = shown.trim() !== '' && Number.isFinite(m) && m >= -50 && m <= 90;
  const applyMargin = () => { command('product.set_pricing', productId, { mode: 'margin', margin: (m / 100).toFixed(3) }); setMargin(''); setChoosing(false); };
  // A granary's grain sells at the store's price, set on the granary's page.
  if (pricing?.granary) return <span className="price-actions">
    <span className="quiet">粮仓的粮按粮仓的卖价卖</span>
    <button className="link" onClick={() => useUIStore.getState().openModal({ kind: 'facility', id: pricing.granary! })}>到粮仓去定</button>
  </span>;
  return <span className="price-actions">
    <span className="seg small pricing-mode">
      <button className={!byMargin && !choosing ? 'active' : ''} onClick={() => { setChoosing(false); if (byMargin) command('product.set_pricing', productId, { mode: 'fixed' }); }}>固定价</button>
      <button className={byMargin || choosing ? 'active' : ''} onClick={() => !byMargin && setChoosing(true)}>按毛利率</button>
    </span>
    {byMargin || choosing ? <>
      <label className="margin-input">毛利率<input value={shown} style={{ width: 44 }} inputMode="decimal" onChange={e => setMargin(e.target.value.replace(/[^0-9.\-]/g, ''))} />%</label>
      {cost > 0 && valid && m < 100 && <span className="quiet">→ {price(cost / (1 - m / 100))}／现价成本 {price(cost)}</span>}
      <button className="link" disabled={!valid || (byMargin && margin === '')} onClick={applyMargin}>{byMargin ? '改毛利率' : '按此毛利率调价'}</button>
      {choosing && <button className="link" onClick={() => { setChoosing(false); setMargin(''); }}>取消</button>}
    </> : <>
      <button className="link" onClick={() => command('product.set_price', productId, { price: (current * .95).toFixed(4) })}>降 5%</button>
      <button className="link" onClick={() => command('product.set_price', productId, { price: (current * 1.05).toFixed(4) })}>提 5%</button>
      <MoneyInput value={value} onChange={setValue} width={60} />
      <button className="link" disabled={!Number(value)} onClick={() => { command('product.set_price', productId, { price: value }); setValue(''); }}>定价</button>
    </>}
  </span>;
}

/** 现价成本 = 各投入品（写明是什么、从哪里来）+ 加工与计件。 */
export function CostLine({ p }: { p: Pricing }) {
  if (p.currentCost === undefined || !p.costParts) return null;
  return <span className="cost-line"><Term name="现价成本">按现在的行情每单位的成本：各投入品现在送到厂里最便宜的来源（粮食比较粮仓、经销商、各地块农户与港口进口，含车马费），加上这条线每单位的加工与计件（昨日的；还没开工时按配方）。按毛利率定价就跟着它每天调价。</Term> {price(p.currentCost)}
    <em>= {p.costParts.map(c => `${c.good} ${price(c.cost)}（${c.source}）`).join(' + ')}{p.workPerUnit !== undefined ? `${p.costParts.length ? ' + ' : ''}加工 ${price(p.workPerUnit)}` : ''}</em></span>;
}

function Products({ c }: { c: Company }) {
  const openModal = useUIStore(s => s.openModal);
  const [designing, setDesigning] = useState(false);
  const productCount = c.products?.length ?? 0;
  return <>
    {c.canPrice && <div className="product-tools">
      {!designing && <button className="action" disabled={productCount >= (c.maxProducts ?? 12) || !c.designable?.length} onClick={() => setDesigning(true)}>新产品设计</button>}
      <span className="quiet">{productCount} / {c.maxProducts ?? 12} 个产品</span>
    </div>}
    {designing && <ProductDesigner c={c} onDone={() => setDesigning(false)} />}
    <BrandsPanel c={c} />
    {!productCount ? <p className="quiet">这家公司没有产品。</p> :
    <ul className="product-rows">{(c.products ?? []).map(p => {
    const margin = p.price > 0 && p.unitCost > 0 ? (p.price - p.unitCost) / p.price : undefined;
    const history = p.history ?? [];
    return <li key={p.productId}>
      <div className="product-main">
        <span className="product-name"><button className="link title" onClick={() => openModal({ kind: 'product', subject: 'product', id: p.productId })}>{p.product}</button>
          <em>{p.good}{p.brand ? <> · <button className="link" onClick={() => openModal({ kind: 'product', subject: 'brand', id: p.brandId! })}>{p.brand}</button></> : ''}</em></span>
        <span className="product-price"><b>{price(p.price)}</b><small>／{p.unit}</small>{p.marginPricing && !p.granary && <em className="pricing-tag">按毛利率 {percent(p.marginTarget ?? 0)}</em>}</span>
        <span className="product-cost">成本 {price(p.unitCost)}{margin !== undefined && <em className={margin < 0 ? 'negative' : ''}>毛利 {percent(margin)}</em>}</span>
        <span className="product-sales">上周 {count(p.lastWeekUnits)} 份 · {pounds(p.lastWeekRevenue)}</span>
        <span className="product-sparks">{history.length > 1 && <><Sparkline values={history.map(h => h.units)} color={palette.teal} label="每周销量" /><Sparkline values={history.map(h => h.price)} color={palette.gold} label="每周价格" /></>}</span>
      </div>
      <CostLine p={p} />
      {p.ai && c.aiRun && !p.marginPricing && <span className="ai-note">AI {p.ai.date} 定价 ×{p.ai.move.toFixed(2)} · 预计日销 {count(p.ai.expected)} / 产能 {count(p.ai.capacity)}</span>}
      <div className="product-actions">
        {c.canPrice && <PriceForm productId={p.productId} current={p.price} pricing={p} />}
        <button className="link" onClick={() => openModal({ kind: 'product', subject: 'product', id: p.productId })}>产品档案</button>
        <button className="link" onClick={() => focusOn('product', p.productId, 'share')}>看份额</button>
      </div>
      <ProductControls c={c} p={p} />
      {p.chain ? <ProductStages p={p} canOperate={!!c.canPrice} /> : <ProductDesign p={p} canOperate={!!c.canPrice} />}
    </li>;
  })}</ul>}
  </>;
}

function FlowBars({ flows, colors }: { flows: Flow[]; colors: string[] }) {
  const sorted = [...flows].sort((a, b) => b.amount - a.amount);
  return <>
    <HBar parts={sorted.map((f, i) => ({ key: f.category, label: f.category, value: f.amount, color: colors[i % colors.length], detail: pounds(f.amount) }))} empty="还没有流水" />
    <dl className="ledger">{sorted.map(f => <Fragment key={f.category}><dt>{f.category}</dt><dd>{pounds(f.amount)}</dd></Fragment>)}</dl>
  </>;
}

function Accounts({ c }: { c: Company }) {
  const rows: ChartRow[] = (c.history ?? []).map(w => ({ date: w.date, income: w.income, spending: w.spending, investment: w.investment }));
  const warm = ['#c9a96a', '#a8875a', '#b98a6e', '#8a7a5c', '#d0b98a', '#9b6f62'], cool = ['#7fb3a8', '#6f8f9a', '#8a7fa8', '#7a8790', '#a3b07a', '#5f7f7a'];
  return <div className="modal-grid two">
    <div className="modal-column">
      <section className="panel-block"><div className="block-head"><h2>每周收支</h2></div>
        <LineChart rows={rows} series={[{ key: 'income', name: '经营收入', color: palette.gold, wide: true }, { key: 'spending', name: '经营开支', color: palette.rust }, { key: 'investment', name: '投资', color: palette.violet, dashed: true }]}
          format={shortPounds} label={`${c.name} 每周收支`} /></section>
      <section className="panel-block"><div className="block-head"><h2><Term name="账目 · 开局以来">每一笔钱都是两个账户之间的转移，或者模型外的流入流出：家庭付款、原料人工、外部承运商。</Term></h2></div>
        <h3>收入</h3><FlowBars flows={c.income ?? []} colors={warm} />
        <h3>支出</h3><FlowBars flows={c.spending ?? []} colors={cool} /></section>
    </div>
    <div className="modal-column">
      {!!c.stock?.length && <section className="panel-block"><div className="block-head"><h2>库存</h2></div><ul className="offer-list compact">{c.stock.map((s, i) => <li key={i}>
        <span><b>{s.product}</b></span><span className="stock">{count(s.quantity)} {s.unit}<em>{s.market}{s.factory ? ' · 工厂仓库' : ''}</em></span></li>)}</ul></section>}
      <section className="panel-block"><div className="block-head"><h2><Term name="在途">在途货物也画在地图上：金色是本公司的货，赭红是经销商运的本公司产品；亮点是走在最前面的一批。</Term></h2><span className="block-note">{c.inTransit?.length ?? 0} 批</span></div>
        {c.inTransit?.length ? <ul className="offer-list compact">{c.inTransit.slice(0, 12).map((s, i) => <li key={i}>
          <span><b>{s.product}</b></span><span className="stock">{count(s.quantity)} {s.unit}<em>{s.from} → {s.to}</em></span><span className="arrive">{s.arrive} 到</span></li>)}</ul> : <p className="quiet">没有在途的货。</p>}
        {(c.inTransit?.length ?? 0) > 12 && <p className="quiet">另有 {(c.inTransit?.length ?? 0) - 12} 批在途。</p>}</section>
    </div>
  </div>;
}

export function CompanyModal({ world, id, tab: initial, onClose }: { world: World; id: string; tab?: string; onClose: () => void }) {
  const [tab, setTab] = useState<Tab>((initial as Tab) ?? 'overview');
  useEffect(() => { if (initial) setTab(initial as Tab); }, [initial]);
  const c = world.company?.id === id ? world.company : undefined;
  const tabs = useMemo(() => c ? [
    { id: 'overview' as Tab, name: '概览' },
    { id: 'works' as Tab, name: '设施', badge: c.facilities.length },
    ...(c.transport ? [{ id: 'transport' as Tab, name: '车队', badge: c.transport.fleets.reduce((n, f) => n + f.units, 0) }] : []),
    ...(c.rail ? [{ id: 'rail' as Tab, name: '线路', badge: c.rail.segments.length }] : []),
    ...(c.mining ? [{ id: 'mining' as Tab, name: '矿权', badge: c.mining.leases.filter(l => l.status === 'active' || l.status === 'pending').length }] : []),
    { id: 'products' as Tab, name: '产品', badge: c.products?.length ?? 0 },
    { id: 'projects' as Tab, name: '项目', badge: c.projects?.length ?? 0 },
    ...(c.knowledge ? [{ id: 'knowledge' as Tab, name: '知识' }] : []),
    { id: 'finance' as Tab, name: '契约' },
    { id: 'accounts' as Tab, name: '账目' },
  ] : [], [c]);
  if (!c) return <Modal eyebrow="公司" title="正在读取公司档案…" onClose={onClose}><p className="quiet">正在读取公司档案…</p></Modal>;
  const rights = c.canMajor ? '可以决定重大事项' : c.canOperate ? '经营这家公司（重大事项需要 75% 表决权）' : c.canVeto ? '可以否决重大事项' : '只读';
  return <Modal className="company-modal" onClose={onClose}
    eyebrow={<>公司 · {c.kind}{c.business ? ` · ${c.business}` : ''}</>}
    title={c.name}
    meta={<><span>注册国 {c.country || '—'}</span><span>控制集团 <em className={c.groupId && c.groupId === world.playerGroup ? 'mine' : ''}>{c.group || '无'}</em></span><span>直接控制人 {c.controller || '无'}</span><span className="readonly">你的集团：{rights}</span></>}
    figures={<><Cell label="现金" value={shortPounds(c.cash)} /><Cell label={c.transport ? '设施与车船账面' : '设施账面'} value={shortPounds(c.bookValue)} /><Cell label="贷款" value={shortPounds(c.debt ?? c.finance?.debt ?? 0)} tone={(c.debt ?? 0) > 0 ? 'negative' : ''} /><Cell label="净资产" value={shortPounds(c.net ?? c.cash + c.bookValue)} /></>}
    actions={<Rename c={c} />}
    tabs={<Tabs tabs={tabs} value={tab} onChange={setTab} />}>
    {tab === 'overview' && <Overview c={c} />}
    {tab === 'works' && <Works c={c} onClose={onClose} />}
    {tab === 'transport' && c.transport && <TransportPanel company={c.id} t={c.transport} canOperate={c.canOperate} day={world.time?.day} />}
    {tab === 'rail' && c.rail && <RailPanel company={c.id} rail={c.rail} day={world.time?.day} />}
    {tab === 'mining' && c.mining && <MiningPanel mining={c.mining} />}
    {tab === 'products' && <Products c={c} />}
    {tab === 'projects' && <div className="modal-grid two"><section className="panel-block"><div className="block-head"><h2><Term name="进行中的项目">单体、长工期、按节点付款、结果有方差：新建、扩建、换产、改型。设备向机器厂按单订购，先付订金、货到付清；也可以从模型外买。</Term></h2></div>
      {c.projects?.length ? <ul className="project-list">{c.projects.map(p => <ProjectRow key={p.id} p={p} canOperate={c.canOperate} />)}</ul> : <p className="quiet">没有进行中的项目。</p>}</section>
      <section className="panel-block"><div className="block-head"><h2>完成的项目</h2></div>{c.projectHistory?.length ? <ul className="notice-list">{c.projectHistory.map((h, i) => <li key={i}>{h.end} · {h.kind}完成{h.cancelled ? '（撤销）' : ''} · {h.name} · {pounds(h.paid)}</li>)}</ul> : <p className="quiet">还没有完成的项目。</p>}</section></div>}
    {tab === 'knowledge' && c.knowledge && <KnowledgePanel company={c.id} k={c.knowledge} canOperate={c.canOperate} />}
    {tab === 'finance' && c.finance && <FinancePanel entity={c.id} f={c.finance} canOperate={c.canOperate} />}
    {tab === 'accounts' && <Accounts c={c} />}
  </Modal>;
}
