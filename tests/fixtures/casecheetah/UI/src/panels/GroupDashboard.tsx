import { useEffect, useState } from 'react';
import { Cell, Term, MoneyInput } from '@/components/Modal';
import { Select } from '@/components/Select';
import { LineChart, Sparkline, palette, type ChartRow } from '@/components/charts';
import { command, count, percent, pounds, shortPounds, signedPounds, statusTone } from '@/lib/format';
import { focusOn } from '@/lib/focus';
import { useUIStore } from '@/stores/ui-store';
import { DividendForm, InjectForm } from './CompanyModal';
import type { CompanyCard, ControlNode, GroupConsole } from '@/types';

function ControlTree({ node }: { node: ControlNode }) {
  const openModal = useUIStore(s => s.openModal);
  const trust = node.kind === '家族信托' || node.kind === '投资信托';
  return <li className={node.controlled ? 'controlled' : ''}>
    <div className="node">
      {trust ? <span className="node-name">{node.name}</span> : <button className="link" onClick={() => openModal({ kind: 'company', id: node.id })}>{node.name}</button>}
      <span>{node.kind}{node.business ? ` · ${node.business}` : ''}</span>
      <span className="figures">{node.stake < 1 ? `持股 ${percent(node.stake)} · ` : ''}{trust ? '' : `表决 ${percent(node.votes)} · `}{pounds(node.cash)}{node.facilities ? ` · ${node.facilities} 处设施` : ''}</span>
    </div>
    {node.children.length > 0 && <ul>{node.children.map(child => <ControlTree key={child.id} node={child} />)}</ul>}
  </li>;
}

function CompanyCardView({ c }: { c: CompanyCard }) {
  const openModal = useUIStore(s => s.openModal);
  const [form, setForm] = useState<'' | 'dividend' | 'inject'>('');
  const history = c.history ?? [];
  return <article className="company-card">
    <div className="company-card-head">
      <button className="link name" onClick={() => openModal({ kind: 'company', id: c.id })}>{c.name}</button>
      <span>{c.kind}{c.business ? ` · ${c.business}` : ''}{c.country ? ` · ${c.country}` : ''}</span>
    </div>
    <div className="company-card-figures">
      <Cell label="现金" value={shortPounds(c.cash)} />
      <Cell label="上周经营" value={c.lastOperating !== undefined ? signedPounds(c.lastOperating) : '—'} tone={(c.lastOperating ?? 0) < 0 ? 'negative' : 'positive'} />
      <div className="card-spark">{history.length > 1 ? <Sparkline values={history.map(w => w.operating)} color={palette.teal} label="每周经营利润" /> : <small>一周后显示走势</small>}</div>
    </div>
    {!!c.works.length && <div className="works-dots">{c.works.map(w => <button key={w.id} className={`dot ${statusTone(w.status)}`} onClick={() => openModal({ kind: 'facility', id: w.id })} title={`${w.name} · ${w.status}${w.ratio >= 0 ? ` · 上周 ${percent(w.ratio)}` : ''}`}>
      <i style={{ width: `${Math.max(0, Math.min(1, w.ratio)) * 100}%` }} /><span>{w.name}</span></button>)}</div>}
    <div className="company-flags">
      {c.research && <span>在研 {c.research}</span>}
      {c.projects > 0 && <span>项目 {c.projects}</span>}
      {c.debt > 0 && <span className="warn">贷款 {shortPounds(c.debt)}</span>}
      {(c.distress ?? 0) > 0 && <span className="warn">困境 {c.distress} 周</span>}
      {c.stake < .999 && <span>经济利益 {percent(c.stake)}</span>}
    </div>
    <div className="company-actions">
      <button className="link" onClick={() => openModal({ kind: 'company', id: c.id })}>经营面板</button>
      {c.canOperate && <button className={`link ${form === 'dividend' ? 'active' : ''}`} onClick={() => setForm(form === 'dividend' ? '' : 'dividend')}>分红</button>}
      {!!c.injectors.length && <button className={`link ${form === 'inject' ? 'active' : ''}`} onClick={() => setForm(form === 'inject' ? '' : 'inject')}>注资</button>}
    </div>
    {form === 'dividend' && <DividendForm company={c.id} cash={c.cash} />}
    {form === 'inject' && <InjectForm company={c.id} injectors={c.injectors} sharePrice={c.sharePrice} canMajor={c.canMajor} />}
  </article>;
}

/** 品牌与产品：每个产品上周的收入、销量与份额，点开产品或品牌档案，或按份额给地图着色。 */
function BrandsOverview({ g }: { g: GroupConsole }) {
  const openModal = useUIStore(s => s.openModal);
  const brands = g.brands ?? [];
  if (!brands.length) return null;
  return <section className="archive-section brands-overview"><h2><Term name="品牌与产品">每个产品上周的收入、销量，以及它在所在需求里的份额（只算它有销量的集散区）。点名字打开产品或品牌档案；“地图”按份额给地图着色。</Term></h2>
    {brands.map(b => <div key={b.id || b.companyId} className="brand-block">
      <div className="brand-head">{b.id ? <button className="link name" onClick={() => openModal({ kind: 'product', subject: 'brand', id: b.id })}>{b.name}</button> : <span className="name">{b.name}</span>}
        <em>{b.company}</em>{b.id && <button className="link" onClick={() => focusOn('brand', b.id, 'share')}>看份额</button>}</div>
      <ul className="brand-products">{b.products.map(p => <li key={p.productId}>
        <span className="product-title"><button className="link" onClick={() => openModal({ kind: 'product', subject: 'product', id: p.productId })}>{p.product}</button><em>{p.good}</em></span>
        <b>{shortPounds(p.revenue)}</b>
        <span className="product-figures">{count(p.units)} 份 · 份额 {percent(p.share)} · {p.markets} 个集散区</span>
        <span className="product-spark">{(p.history?.length ?? 0) > 1 ? <Sparkline values={p.history.map(w => w.revenue)} label="每周收入" /> : null}</span>
        <button className="link" onClick={() => focusOn('product', p.productId, 'share')}>看份额</button>
      </li>)}</ul>
    </div>)}
  </section>;
}

/** 设立：信托或控股公司出资设立全资的经营公司。 */
function FoundCompany({ g }: { g: GroupConsole }) {
  const founders = g.founders ?? [];
  const [founder, setFounder] = useState(founders[0]?.id ?? '');
  const [business, setBusiness] = useState(g.businesses?.[0]?.id ?? '');
  const [country, setCountry] = useState('');
  const [name, setName] = useState('');
  const [capital, setCapital] = useState('5000');
  const parent = founders.find(f => f.id === founder);
  useEffect(() => { if (parent && !country) setCountry(parent.countryId); }, [parent, country]);
  const types = g.businesses?.find(b => b.id === business)?.types ?? [];
  const ok = !!parent && !!business && name.trim().length > 0 && Number(capital) >= 100 && Number(capital) <= parent.cash;
  return <section className="archive-section found-company"><h2><Term name="设立公司">经营公司只做一种业务；垂直整合要在集团里开姊妹公司。出资方是你的家族信托或控股公司，新公司全资持有，注册国决定关税、法律和战时会不会被没收。</Term></h2>
    <div className="form-grid">
      <div className="field"><span>出资方</span><Select value={founder} onChange={setFounder} ariaLabel="出资方" options={founders.map(f => ({ value: f.id, label: `${f.name}（${shortPounds(f.cash)}）` }))} /></div>
      <label>名称（例如：惠特洛煤铁公司）<input value={name} onChange={e => setName(e.target.value)} /></label>
      <div className="field"><span>业务</span><Select value={business} onChange={setBusiness} ariaLabel="业务" options={(g.businesses ?? []).map(b => ({ value: b.id, label: b.name }))} /></div>
      <div className="field"><span>注册国</span><Select value={country} onChange={setCountry} ariaLabel="注册国" options={(g.countries ?? []).map(c => ({ value: c.id, label: c.name || c.id }))} /></div>
      <div className="field"><span>资本金</span><span className="capital-row"><MoneyInput value={capital} onChange={setCapital} />{[2000, 5000, 10000, 20000].map(v => <button key={v} className="link" onClick={() => setCapital(String(v))}>{shortPounds(v)}</button>)}</span></div>
    </div>
    {!!types.length && <p className="quiet">这种业务能建：{types.join('、')}。</p>}
    <button className="action" disabled={!ok} onClick={() => { command('corp.found_company', founder, { name: name.trim(), business, country, capital }); setName(''); }}>出资 {pounds(Number(capital) || 0)} 设立</button>
    {parent && Number(capital) > parent.cash && <p className="quote-reason">{parent.name}的现金不够。</p>}
  </section>;
}

export function GroupDashboard({ g }: { g: GroupConsole }) {
  const history = g.history ?? [];
  const rows: ChartRow[] = history.map(w => ({ date: w.date, wealth: w.wealth, power: w.power, family: w.familyCash }));
  const spark = (key: 'familyCash' | 'wealth' | 'power') => history.length > 1 ? <Sparkline values={history.slice(-26).map(w => w[key])} label="近 26 周" /> : null;
  const companies = g.companies ?? [];
  const openModal = useUIStore(s => s.openModal);
  return <>
    <div className="group-figures-row">
      <div className="group-figure"><Term name="家族资金">家族信托里的现金：意志自己的钱包。集团本身没有资产负债表。</Term><b>{pounds(g.familyCash)}</b>{spark('familyCash')}</div>
      <div className="group-figure"><Term name="财富">按持股比例计算的经济利益：沿每条持股路径把比例相乘再相加。</Term><b>{pounds(g.wealth)}</b>{spark('wealth')}</div>
      <div className="group-figure"><Term name="权力">实际控制的资产：最终控制人是本集团的所有法人的现金与设施账面。</Term><b>{pounds(g.power)}</b>{spark('power')}</div>
    </div>
    <section className="archive-section"><h2>权力与财富</h2>
      <LineChart rows={rows} series={[{ key: 'power', name: '权力', color: palette.gold, wide: true }, { key: 'wealth', name: '财富', color: palette.teal }, { key: 'family', name: '家族资金', color: palette.ivory, dashed: true }]}
        format={shortPounds} label="集团每周的权力、财富与家族资金" height={180} />
    </section>
    <section className="archive-section"><h2>旗下公司<span className="section-count">{companies.length} 家</span></h2>
      {!!companies.length && <div className="place-actions"><button className="brush-button" onClick={() => openModal({ kind: 'tech-tree', company: companies[0].id })}>科技树</button></div>}
      <div className="company-cards">{companies.map(c => <CompanyCardView key={c.id} c={c} />)}</div>
      {!companies.length && <p className="quiet">集团还没有控制任何公司。</p>}
    </section>
    {!!g.supply?.length && <section className="archive-section group-supply"><h2><Term name="集团内供需">集团里一家公司做的正是另一家要用的：地图“版图”上画成两处建筑之间的线。上周用的那家从集团内进了多少、从别处进了多少。</Term></h2>
      <ul className="supply-list">{g.supply.map(s => <li key={`${s.fromId}>${s.toId}:${s.good}`}>
        <span className="supply-pair"><button className="link" onClick={() => focusOn('company', s.fromId)}>{s.from}</button><i>→</i>
          <button className="good-chip" onClick={() => focusOn('good', s.good)}><img src={`art/goods/${s.good}.png`} alt="" /><span>{s.goodName}</span></button><i>→</i>
          <button className="link" onClick={() => focusOn('company', s.toId)}>{s.to}</button></span>
        <em>上周集团内 {count(s.inside)} {s.unit} · 别处 {count(s.outside)} {s.unit}</em></li>)}</ul>
    </section>}
    <BrandsOverview g={g} />
    {!!g.founders?.length && <FoundCompany g={g} />}
    <section className="archive-section"><h2>控制路径</h2>{g.tree ? <ul className="control-tree"><ControlTree node={g.tree} /></ul> : <p className="quiet">没有家族信托。</p>}</section>
    {!!g.seizures?.length && <section className="archive-section"><h2><Term name="银行待售的抵押股">贷款违约后，银行收走抵押的股份；持有期间不投票，限期内必须卖出。谁手里有现金，谁就能趁机买下控制权；到期没人买，就卖给散户。</Term></h2>
      <ul className="option-list">{g.seizures.map(s => <li key={s.id}><span><b>{s.entity} {count(s.shares)} 股</b><em>{s.bank}收自{s.borrower} · 要价 {pounds(s.ask)} · {s.deadline} 前</em></span>
        {g.familyTrustId && <button className="link" onClick={() => command('finance.buy_seized', s.id, { buyer: g.familyTrustId! })}>由家族信托买下</button>}</li>)}</ul></section>}
  </>;
}
