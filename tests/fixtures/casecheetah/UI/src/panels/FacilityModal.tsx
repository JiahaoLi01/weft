import { DepotPanel } from './Depot';
import { HousePanel } from './House';
import { GranaryPanel } from './Granary';
import { Fragment, useEffect, useMemo, useState } from 'react';
import { AllocationGood, AllocationPanel } from './ProductOps';
import { Modal, Cell, StatusBadge, Tabs, Term } from '@/components/Modal';
import { Select } from '@/components/Select';
import { CraftBars, Figure, HBar, LineChart, Meter, StatPairs, UtilBars, palette, type ChartRow, type Series } from '@/components/charts';
import { command, count, decimal, focusFacility, percent, perUnit, pounds, price, shortPounds, statusTone } from '@/lib/format';
import { useUIStore } from '@/stores/ui-store';
import { ProjectRow, QuoteText, craftsText } from './Step4';
import { ChainFlow } from './Stages';
import { tons } from './Mining';
import type { FacilityDetail, LineCard, Quote, World } from '@/types';

// 工厂面板（《生产设计》“产线”）：一处工厂由几条产线组成，规模 = 产线数；一条线只做一种商品，有自己的生产方式、
// 班次、班组与工程。上方是各条线的一览，下方是选中那条线的详情；扩建就是“新增一条产线”。

/** Cost parts of a quote as a proportional bar. */
export function QuoteBar({ q }: { q: Quote }) {
  return <HBar parts={[
    { key: 'building', label: '建筑', value: q.building, color: '#a8875a', detail: pounds(q.building) },
    { key: 'machines', label: '机器', value: q.machines, color: '#6f9a92', detail: pounds(q.machines) },
    { key: 'install', label: '安装', value: q.install, color: '#8a7fa8', detail: pounds(q.install) },
    { key: 'engineering', label: '工程验证', value: q.engineering, color: '#9b6f62', detail: pounds(q.engineering) },
  ]} />;
}

const weeks = (q: Quote) => `约 ${Math.max(1, Math.round(q.days / 7))} 周`;

/** A line's products: names, with shares when several share it. */
function PartsText({ l }: { l: LineCard }) {
  if (!l.parts.length) return <>还没有{l.good}产品</>;
  return <>{l.parts.map((p, i) => <Fragment key={p.productId}>{i > 0 && '、'}{p.product}{l.parts.length > 1 ? ` ${percent(p.share)}` : ''}</Fragment>)}</>;
}

/** 设施卡片上的各条线（地块与公司的设施列表）：号线、商品、班次、昨日产出／额定。 */
export function LineBriefs({ lines }: { lines: LineCard[] }) {
  return <ul className="line-briefs">{lines.map(l => <li key={l.id} className={statusTone(l.status)}>
    <b>{l.name}</b><span>{l.good}</span><span>{l.shiftName ?? ''}</span>
    <span>{l.nominal ? `${decimal(l.produced ?? 0)} / ${decimal(l.nominal)} ${l.unit}` : l.status ?? ''}</span>
  </li>)}</ul>;
}

// ---- 产线一览 ----

type Pick = number | 'new';

/** 全厂排班：把每条线排成同一个班次（各线也可以单独排）。 */
function AllShifts({ f }: { f: FacilityDetail }) {
  const lines = f.lines ?? [];
  const shifts = lines.find(l => l.shifts?.length)?.shifts ?? [];
  if (!f.canOperate || lines.length < 2 || !shifts.length) return null;
  const same = lines.every(l => l.shift === lines[0].shift) ? lines[0].shift : undefined;
  return <span className="all-shifts">全厂排班
    <span className="seg small">{shifts.map(s => <button key={s.id} className={same === s.id ? 'active' : ''} aria-pressed={same === s.id} disabled={same === s.id}
      onClick={() => command('facility.set_shift', f.id, { shift: s.id })}>{s.name}</button>)}</span></span>;
}

function LineStrip({ f, pick, onPick }: { f: FacilityDetail; pick: Pick; onPick: (p: Pick) => void }) {
  const lines = f.lines ?? [];
  const expansion = f.project;
  const canAdd = f.canOperate && !!f.options?.expand.length;
  return <section className="panel-block">
    <div className="line-toolbar">
      <h2><Term name="产线">一条产线只做一种商品，同一设计的几个产品可以分它的产量；生产方式、班次与班组按线安排，一条线换产安装时别的线照常生产。工厂的规模就是产线数，扩建就是多开一条线。</Term></h2>
      <span className="block-note">{lines.length} 条{f.canOperate ? ' · 点一条线看详情' : ''}</span>
      <AllShifts f={f} />
    </div>
    <div className="line-strip">
      {lines.map(l => {
        const status = l.status ?? '';
        const short = l.heads !== undefined && l.required !== undefined && l.heads < l.required;
        const ran = !!l.nominal;
        return <button key={l.id} className={`line-row ${pick === l.id ? 'active' : ''}`} aria-pressed={pick === l.id} onClick={() => onPick(l.id)}>
          <span className="line-name"><b>{l.name}</b>{l.size < .999 && <em>规模 {decimal(l.size)}</em>}</span>
          <span className="line-good"><b>{l.good}</b><em><PartsText l={l} /></em></span>
          <span className="line-status">{status && <StatusBadge status={status} tone={statusTone(status)} />}</span>
          <span className="line-stats">
            <span className="s-shift">{l.shiftName ?? '—'}</span>
            <span className="s-output">{ran ? <><UtilBars ratio={(l.produced ?? 0) / (l.nominal ?? 1)} segments={10} />{decimal(l.produced ?? 0)} / {decimal(l.nominal ?? 0)} {l.unit}</> : <em>昨日没有开工</em>}</span>
            <span className={`s-crew ${short ? 'negative' : ''}`}><em>人手</em>{l.required !== undefined ? `${count(l.heads ?? 0)} / ${count(l.required)}` : '—'}</span>
            <span className="s-skill"><em>熟练</em>{ran && l.effective !== undefined ? percent(l.effective) : '—'}</span>
          </span>
        </button>;
      })}
      {expansion && <button className={`line-row add ${pick === 'new' ? 'active' : ''}`} aria-pressed={pick === 'new'} onClick={() => onPick('new')}>
        <span className="line-name"><b>新线</b></span>
        <span className="line-good"><b>{expansion.name}</b><em>{expansion.phase}{expansion.progress !== undefined ? ` ${percent(expansion.progress)}` : ''}{expansion.phaseEnd ? ` · 预计 ${expansion.phaseEnd}` : ''}</em></span>
      </button>}
      {!expansion && canAdd && <button className={`line-row add ${pick === 'new' ? 'active' : ''}`} aria-pressed={pick === 'new'} onClick={() => onPick('new')}>
        <span className="line-name"><b>＋</b></span>
        <span className="line-good"><b>新增一条产线</b><em>扩建：可以做这类工厂会做的任何一种商品</em></span>
      </button>}
    </div>
  </section>;
}

// ---- 选中的一条线 ----

type LineTab = 'overview' | 'products' | 'crews' | 'method';

/** 班次对比：这条线的设备与工人在半班、单班、双班下的人手、额定产量与开支。 */
function ShiftPanel({ f, l }: { f: FacilityDetail; l: LineCard }) {
  const shifts = l.shifts ?? [];
  const locked = !f.canOperate || !!l.project || l.idle;
  return <section className="panel-block shift-panel">
    <div className="shift-head"><h2><Term name="班次">设备是整块买的、按年折旧，闲着就是亏；人手可以增减：订单多时两班倒，少时减为半班，比买卖设备快得多。双班人手与工资翻倍、执行质量略降。班次按线排，只改这条线。</Term></h2>
      <div className="seg">{shifts.map(s => <button key={s.id} className={s.current ? 'active' : ''} aria-pressed={s.current} disabled={locked || s.current}
        onClick={() => command('facility.set_shift', f.id, { shift: s.id, line: String(l.id) })}>{s.name}</button>)}</div></div>
    <div className="shift-grid" style={{ gridTemplateColumns: `auto repeat(${shifts.length}, minmax(0, 1fr))` }}>
      <span />{shifts.map(s => <span key={s.id} className={s.current ? 'cur' : ''}>{s.name}</span>)}
      <span>额定／日</span>{shifts.map(s => <span key={s.id} className={s.current ? 'cur' : ''}>{decimal(s.nominal[0] ?? 0)} {l.unit}</span>)}
      <span>人手</span>{shifts.map(s => <span key={s.id} className={s.current ? 'cur' : ''}>{count(s.required)}</span>)}
      <span>工资／日</span>{shifts.map(s => <span key={s.id} className={s.current ? 'cur' : ''}>{pounds(s.wages)}</span>)}
      <span>维护／日</span>{shifts.map(s => <span key={s.id} className={s.current ? 'cur' : ''}>{pounds(s.upkeep)}</span>)}
      <span>一次性</span>{shifts.map(s => <span key={s.id} className={s.current ? 'cur' : ''}>{s.current ? '当前' : s.hire > 0 ? `招聘 ${pounds(s.hire)}` : '—'}</span>)}
      <span>执行质量</span>{shifts.map(s => <span key={s.id} className={s.current ? 'cur' : ''}>{s.quality > 0 ? `+${s.quality}` : s.quality < 0 ? `${s.quality}` : '±0'}</span>)}
    </div>
  </section>;
}

type Metric = 'output' | 'skill' | 'cost' | 'crew';

function LineHistory({ f, l }: { f: FacilityDetail; l: LineCard }) {
  const [metric, setMetric] = useState<Metric>('output');
  const history = l.history ?? [];
  const { rows, series, format } = useMemo(() => {
    const rows: ChartRow[] = history.map(w => {
      const r: ChartRow = { date: w.date, shade: w.project };
      r.produced = w.produced[0] ?? 0;
      r.nominal = w.nominal[0] ?? 0;
      r.effective = w.effective * 100;
      if (w.unitCost !== undefined) r.cost = w.unitCost;
      r.heads = w.heads;
      r.required = w.required;
      return r;
    });
    let series: Series[];
    let format = (v: number) => count(v);
    if (metric === 'output') series = [{ key: 'produced', name: '实产', color: palette.gold, wide: true }, { key: 'nominal', name: '额定', color: palette.gold, dashed: true }];
    else if (metric === 'skill') { series = [{ key: 'effective', name: '有效熟练度', color: palette.teal, wide: true }]; format = v => `${Math.round(v)}%`; }
    else if (metric === 'cost') { series = [{ key: 'cost', name: '单位成本（含固定开支）', color: palette.rust, wide: true }]; format = v => price(v); }
    else series = [{ key: 'heads', name: '在岗', color: palette.gold, wide: true }, { key: 'required', name: '需要', color: palette.ivory, dashed: true }];
    return { rows, series, format };
  }, [history, metric]);
  const tabs: { id: Metric; name: string }[] = [{ id: 'output', name: '每周产量' }, { id: 'skill', name: '熟练度' }, { id: 'cost', name: '单位成本' }, { id: 'crew', name: '人手' }];
  return <section className="panel-block">
    <div className="block-head"><h2>周史</h2><div className="seg small">{tabs.map(t => <button key={t.id} className={metric === t.id ? 'active' : ''} onClick={() => setMetric(t.id)}>{t.name}</button>)}</div></div>
    <LineChart rows={rows} series={series} format={format} label={`${f.name} ${l.name} 每周${tabs.find(t => t.id === metric)?.name}`} empty="推进一周后显示这条线的周史。"
      unitHint={history.some(w => w.project) ? '淡金背景：工程进行中的周' : undefined} />
  </section>;
}

/** 进货：每种投入品从哪里买、买了多少、平均每单位多少钱（含车马）。 */
function Purchases({ l }: { l: LineCard }) {
  const [when, setWhen] = useState<'day' | 'week' | 'last'>('day');
  const rows = (when === 'day' ? l.bought : when === 'week' ? l.weekBought : l.lastWeekBought) ?? [];
  const tabs: { id: typeof when; name: string }[] = [{ id: 'day', name: '昨日' }, { id: 'week', name: '本周' }, { id: 'last', name: '上周' }];
  if (!l.inputs?.length) return null;
  return <section className="panel-block">
    <div className="block-head"><h2><Term name="进货">每种投入品从哪里买的：自家存货、别家的现货（粮仓的粮另付从仓里运来的车马费）、地块上的农户（地头价加车马费）、港口进口或模型外。每单位的价钱含车马费。</Term></h2>
      <div className="seg small">{tabs.map(t => <button key={t.id} className={when === t.id ? 'active' : ''} onClick={() => setWhen(t.id)}>{t.name}</button>)}</div></div>
    {rows.length ? <div className="table purchases"><span className="th">投入品</span><span className="th">从哪里</span><span className="th">数量</span><span className="th">每单位（含车马）</span>
      {rows.map((b, i) => <Fragment key={i}><span>{b.good}</span><span>{b.source}</span><span>{decimal(b.quantity)} {b.unit}</span>
        <span>{b.quantity > 0 ? price(b.cost / b.quantity) : '—'}{b.haul > 0 && <em className="detail-note">车马 {price(b.haul / b.quantity)}</em>}</span></Fragment>)}
    </div> : <p className="quiet">{when === 'day' ? '昨日没有进货。' : when === 'week' ? '本周还没有进货。' : '上周没有进货。'}</p>}
  </section>;
}

/** 概况：昨日的产出与开支、班次，周史、成本构成与投入品。 */
function LineOverview({ f, l }: { f: FacilityDetail; l: LineCard }) {
  if (l.shift === undefined) return <p className="quiet">这条线刚建成，明天起招工投产。</p>;
  const spent = (l.materials ?? 0) + (l.conversion ?? 0) + (l.piece ?? 0) + (l.wages ?? 0) + (l.upkeep ?? 0);
  // 手艺：the line's crafts at its crews' proficiency; the weakest is what a change of way costs most.
  const crafts = l.crafts ?? [];
  const mean = crafts.length ? crafts.reduce((a, c) => a + c.value, 0) / crafts.length : 0;
  const weakest = crafts.reduce<typeof crafts[number] | undefined>((a, c) => (!a || c.value < a.value ? c : a), undefined);
  const market = new Map((f.inputs ?? []).map(i => [i.good, i]));
  // 材料 by input: yesterday's purchases of each, so a mill's cost reads 稻谷 rather than 材料.
  const byGood = new Map<string, { cost: number; quantity: number; unit: string }>();
  for (const b of l.bought ?? []) {
    const g = byGood.get(b.good) ?? { cost: 0, quantity: 0, unit: b.unit };
    g.cost += b.cost; g.quantity += b.quantity; byGood.set(b.good, g);
  }
  const materials = byGood.size ? [...byGood].map(([good, g], i) => ({ key: `m${i}`, label: good, value: g.cost, color: ['#a8875a', '#b89a6a', '#97764c'][i % 3],
    detail: `${pounds(g.cost)} · ${decimal(g.quantity)} ${g.unit}` }))
    : [{ key: 'materials', label: '材料', value: l.materials ?? 0, color: '#a8875a', detail: pounds(l.materials ?? 0) }];
  return <div className="modal-grid two">
    <div className="modal-column">
      <section className="panel-block">
        <div className="block-head"><h2>昨日</h2><span className="block-note">{l.shiftName} · {l.process}</span></div>
        <div className="production-readout">
          <Figure value={decimal(l.produced ?? 0)} caption={<>{l.good}昨日实产（{l.unit}）<br />额定 {decimal(l.nominal ?? 0)} {l.unit}／日</>} />
          <UtilBars ratio={l.nominal ? (l.produced ?? 0) / l.nominal : 0} />
        </div>
        <div className="line-metrics">
          <Cell label="人手" value={`${count(l.heads ?? 0)} / ${count(l.required ?? 0)}`} tone={(l.heads ?? 0) < (l.required ?? 0) ? 'negative' : ''} />
          <Cell label="有效熟练度" value={percent(l.effective ?? 0)} tone={Math.min(l.effective ?? 0, l.pace ?? 1) < .82 ? 'negative' : 'positive'}
            note={l.pace !== undefined && l.pace < (l.effective ?? 0) - .005 ? `最慢一道只到 ${percent(l.pace)}` : undefined} />
          <Cell label="单位成本" value={l.unitCost ? price(l.unitCost) : '—'} note="材料＋加工＋计件" />
          <Cell label="昨日开支" value={pounds(spent)} note={`工资 ${pounds(l.wages ?? 0)} · 维护 ${pounds(l.upkeep ?? 0)}`} />
        </div>
        {!!crafts.length && (l.heads ?? 0) > 0 && <Meter label="手艺（在用，按人数平均）" value={mean / 100}
          text={`${Math.round(mean)}${crafts.length > 1 && weakest ? ` · 最弱 ${weakest.name} ${Math.round(weakest.value)}` : ''}`} />}
        {l.limit && <p className="line-limit">受限：{l.limit}</p>}
      </section>
      <ShiftPanel f={f} l={l} />
    </div>
    <div className="modal-column">
      <LineHistory f={f} l={l} />
      <section className="panel-block">
        <div className="block-head"><h2><Term name="昨日成本">可变成本 = 材料 + 加工（工艺、经验与熟练度）+ 计件工资；固定开支 = 班组工资 + 维护。折旧另计，冲减设施账面。</Term></h2><span className="block-note">{pounds(spent)}</span></div>
        <HBar parts={[
          ...materials,
          { key: 'conversion', label: '加工', value: l.conversion ?? 0, color: '#6f9a92', detail: pounds(l.conversion ?? 0) },
          { key: 'piece', label: '计件', value: l.piece ?? 0, color: '#8a7fa8', detail: pounds(l.piece ?? 0) },
          { key: 'wages', label: '工资', value: l.wages ?? 0, color: '#9b6f62', detail: pounds(l.wages ?? 0) },
          { key: 'upkeep', label: '维护', value: l.upkeep ?? 0, color: '#7a8790', detail: pounds(l.upkeep ?? 0) },
        ]} empty="昨日没有开支" />
      </section>
      {!!l.inputs?.length && <section className="panel-block">
        <div className="block-head"><h2><Term name="投入品">先用自己在本集散区的存货，再按送到厂里的价钱从低到高买：粮食比较粮仓、经销商的现货（粮仓的加上从仓里运来的车马费）、各地块农户的地头价加车马费、港口进口；别的投入品买区内不贵于模型外价的现货，再按模型外价格买。投入品的质量传到产品。</Term></h2><span className="block-note">每 1 {l.unit}{l.good}</span></div>
        <div className="table inputs"><span className="th">投入品</span><span className="th">每单位耗</span><span className="th">现在最便宜（到厂）</span><span className="th">折合每{l.unit}</span><span className="th">区内现货</span>
          {l.inputs.map(i => { const m = market.get(i.good); return <Fragment key={i.good}><span>{i.good}</span><span>{perUnit(i.perUnit)} {i.unit}</span>
            <span>{i.price ? <>{price(i.price)}／{i.unit}<em className="detail-note">{i.source}</em></> : '—'}</span>
            <span>{i.cost ? price(i.cost) : '—'}</span><span>{m ? `${count(m.local)} ${i.unit}` : '—'}</span></Fragment>; })}
        </div>
      </section>}
      <Purchases l={l} />
    </div>
  </div>;
}

/** 产品：这条线做哪些产品、各占几成，工厂仓库里的存货。 */
function LineProducts({ f, l }: { f: FacilityDetail; l: LineCard }) {
  const stock = (f.warehouse ?? []).filter(w => l.parts.some(p => p.product === w.product));
  return <div className="modal-grid two">
    <section className="panel-block allocation">
      <div className="block-head"><h2><Term name="产线分配">这条线做哪些产品、各占几成。一条线只做一种商品；设计相同的产品共用一套工装，份额随时可调，明天起生效；设计不同的产品（包括别的商品）要先换产。</Term></h2></div>
      {l.allocation ? <AllocationGood f={f} a={l.allocation} line={l.id} /> : <>
        <HBar parts={l.parts.map((p, i) => ({ key: p.productId, label: p.product, value: Math.max(p.share, .0001), color: [palette.gold, palette.teal, palette.violet, palette.sand][i % 4], detail: percent(p.share) }))} empty={`还没有${l.good}产品`} />
        <StatPairs rows={l.parts.map(p => [p.product, `${p.brand || '无品牌'} · ${percent(p.share)}`])} />
      </>}
    </section>
    {!!stock.length && <section className="panel-block">
      <div className="block-head"><h2><Term name="工厂仓库">产出先进工厂仓库，再按缺口分给本地直销仓和各经销商。仓库最多存额定产量的 30 天，满了就减产。同一产品在几条线上做时共用一个仓位。</Term></h2></div>
      {stock.map(w => <Meter key={w.product} label={w.product} value={w.cap > 0 ? w.quantity / w.cap : 0} text={`${count(w.quantity)} / ${count(w.cap)} ${w.unit}`} tone={w.cap > 0 && w.quantity / w.cap > .9 ? 'warn' : ''} />)}
    </section>}
  </div>;
}

/** 班组：这条线的班组，调人到同一集散区、同一家公司的另一条线。 */
function CrewBlock({ f, l }: { f: FacilityDetail; l: LineCard }) {
  const targets = useMemo(() => (f.transfer ?? []).filter(t => t.id !== l.key), [f.transfer, l.key]);
  const [to, setTo] = useState(targets[0]?.id ?? '');
  const [heads, setHeads] = useState('10');
  useEffect(() => { if (!targets.some(t => t.id === to)) setTo(targets[0]?.id ?? ''); }, [targets, to]);
  const crews = l.crews ?? [];
  const total = crews.reduce((a, c) => a + c.heads, 0);
  return <section className="panel-block">
    <div className="block-head"><h2><Term name="班组">一群一起干活的工人，从地块人才池招来。班组的手艺就是它做过的生产技术：在用的越做越熟（向 100），不用的慢慢忘掉；换了做法，新做法的熟练从会的做法按技术距离折算（竹筛 90 → 转筒筛 71）。人数守恒，可以分出、合并、调到同一集散区的另一条线（本厂的别的线也行）；一条线上手艺相近的班组会并成一个。</Term></h2>
      <span className="block-note">{crews.length} 个 · {count(total)} / {count(l.required ?? 0)} 人</span></div>
    {crews.length ? <ul className="crew-cards wide">{crews.map(c => <li key={c.id}>
      <div className="crew-head"><b>{count(c.heads)} 人</b><span>来自{c.origin} · 有效 {percent(c.effective)}</span></div>
      <CraftBars crafts={c.crafts} />
    </li>)}</ul> : <p className="quiet">{l.idle ? '封存的线没有班组。' : '还没有班组：明天起从地块招工。'}</p>}
    {f.canOperate && !!targets.length && !!crews.length && <div className="inline-form">
      <span>从这条线调</span>
      <input value={heads} onChange={e => setHeads(e.target.value.replace(/[^0-9]/g, ''))} inputMode="numeric" style={{ width: 52 }} /><span>人到</span>
      <Select value={to} onChange={setTo} ariaLabel="调入的产线" options={targets.map(t => ({ value: t.id, label: `${t.name}（${t.heads}/${t.required} 人）` }))} />
      <button className="link" disabled={!to || !Number(heads)} onClick={() => command('crew.transfer', f.id, { line: String(l.id), to, heads })}>调过去</button>
    </div>}
    {f.canOperate && !targets.length && <p className="quiet">同一集散区里没有这家公司的其他产线，暂时不能调人。</p>}
  </section>;
}

/** 生产方式：换产到别的生产方式，对比当前与换产后；代价按设计距离算。 */
function MethodPanel({ f, l }: { f: FacilityDetail; l: LineCard }) {
  const methods = l.methods ?? [];
  const [method, setMethod] = useState(methods.find(m => m.known)?.id ?? methods[0]?.id ?? '');
  const chosen = methods.find(m => m.id === method);
  const current = l.current;
  return <div className="modal-grid two">
    <section className="panel-block engineering">
      <div className="block-head"><h2><Term name="生产方式">物化在机器里的知识：机床专用度省人手和手艺，精度传递执行质量；蒸汽机功率提高产量，经济性省煤。换产只动这一条线：报废部分机床、付安装费、停产几周，工人对新方式的手艺从旧方式按技术距离折算，别的线照常生产。</Term></h2></div>
      <StatPairs rows={[['当前', l.method ?? '默认生产方式'], ['工艺', l.process]]} />
      {!f.canOperate ? <p className="quiet">只有经营这家公司的集团可以换产。</p>
        : l.project ? <p className="quiet">这条线有工程在进行，完工后才能再换产。</p>
        : methods.length ? <div className="method-list">{methods.map(m => <button key={m.id} className={`method ${m.id === method ? 'active' : ''} ${m.known ? '' : 'unknown'}`} onClick={() => setMethod(m.id)}>
          <b>{m.name}</b><span>{m.known ? m.effects : '还没有掌握'}</span></button>)}</div>
        : <p className="quiet">这类工厂没有别的生产方式。</p>}
    </section>
    {f.canOperate && !l.project && chosen && <section className="panel-block">
      <div className="option-head"><b>换产为{chosen.name}</b><span>{l.name} · {l.good}</span></div>
      {chosen.quote ? chosen.quote.ok ? <>
        <QuoteBar q={chosen.quote} />
        {current && chosen.after && <>
          <div className="compare-label"><span>指标（{l.shiftName ?? '当前班次'}）</span><span>当前 → 换产后</span></div>
          <StatPairs rows={[
            ['额定／日', `${decimal(current.nominal)} → ${decimal(chosen.after.nominal)} ${l.unit}`, chosen.after.nominal > current.nominal ? 'positive' : chosen.after.nominal < current.nominal ? 'negative' : ''],
            ['人手', `${count(current.required)} → ${count(chosen.after.required)}`, chosen.after.required < current.required ? 'positive' : chosen.after.required > current.required ? 'negative' : ''],
            ['工资／日', `${pounds(current.wages)} → ${pounds(chosen.after.wages)}`],
            ['维护／日', `${pounds(current.upkeep)} → ${pounds(chosen.after.upkeep)}`]]} />
        </>}
        <div className="compare-label"><span>指标</span><span>换产的代价</span></div>
        <StatPairs rows={[['报价', pounds(chosen.quote.total)], ['当付', pounds(chosen.quote.dueNow)], ['停产安装', weeks(chosen.quote)],
          ['设计距离 D', chosen.quote.distance.toFixed(2)], ['报废机床', percent(chosen.quote.scrap), chosen.quote.scrap > .3 ? 'negative' : ''],
          ['工人手艺', craftsText(chosen.quote) || '不变'], ['注销账面', pounds(chosen.quote.writeOff)]]} />
        <p className="quiet">效果：{chosen.effects}</p>
        <button className="action" onClick={() => command('project.retool', f.id, { line: String(l.id), method: chosen.id })}>{l.name}换产为{chosen.name}</button>
      </> : <QuoteText q={chosen.quote} /> : <p className="quote-reason">还没有掌握这种生产方式：先在公司的“知识与研发”里研发、逆向或取得授权。</p>}
    </section>}
  </div>;
}

/** 一条线怎么做：工序形态是它做的产品的工序链，别的是它的生产方式。 */
function methodText(l: LineCard) {
  if (l.chain) return l.chain.stages.filter(s => s.present).map(s => s.wayName).join(' · ');
  return l.method ?? '默认生产方式';
}

/**
 * 工序（Processes.md 第 9 节）：这条线照它做的产品的工序链干活，这里是在这处工厂的实际每级能力与人手；能力最小的那道是瓶颈。
 * 工序是产品的设计，在产品的“改工序”里改（做它的各条线一起换）；动力、传送是整座工厂的，在“设施”页签里换。
 */
function ChainPanel({ f, l }: { f: FacilityDetail; l: LineCard }) {
  const openModal = useUIStore(s => s.openModal);
  const chain = l.chain;
  if (!chain) return null;
  const necks = chain.stages.filter(s => s.bottleneck).map(s => s.name);
  return <section className="panel-block">
    <div className="block-head"><h2><Term name="工序">一种商品的加工是一串工序，每道一种做法：硬手工人人会，机器做法要掌握技术、买机器，还要够档的动力。能力最小的那道定这条线的产量（瓶颈），上游等下游，其余各道也只做这么多。每道的实际能力 = 额定 × (0.5 + 0.5 × 班组对这道做法的熟练度)：换了做法，熟练从旧做法折算，慢慢爬回来；人手是各道的加上搬运。工序链是产品的设计：经过清理、蒸谷、抛光、分级的米已经是另一种商品。</Term></h2>
      <span className="block-note">{chain.product ? `照「${chain.product}」的工序做` : ''}</span></div>
    <ChainFlow chain={chain} unit={l.unit} footer={<>每级 <b>{decimal(chain.actual ?? chain.capacity)}</b> {l.unit}／日{chain.actual !== undefined && Math.abs(chain.actual - chain.capacity) > .5 ? `（额定 ${decimal(chain.capacity)}）` : ''} · {count(chain.crew)} 人{chain.porters ? `（含搬运 ${count(chain.porters)}）` : ''}
      {necks.length ? ` · 瓶颈：${necks.join('、')}` : ''} · 动力 {chain.power ?? '—'} · 传送 {chain.conveyance ?? '—'}
      <em>要改工序，到{chain.product ? `「${chain.product}」` : '产品'}的“改工序”里改型，做它的各条线一起换；动力、传送在这处工厂的“设施”页签里换。</em></>} />
    <button className="link flow-link" onClick={() => openModal({ kind: 'company', id: f.companyId, tab: 'products' })}>到公司的产品页签改工序</button>
  </section>;
}

function LineDetail({ f, l, tab, onTab }: { f: FacilityDetail; l: LineCard; tab: LineTab; onTab: (t: LineTab) => void }) {
  const tabs: { id: LineTab; name: string; badge?: React.ReactNode }[] = [
    { id: 'overview', name: '概况' }, { id: 'products', name: '产品', badge: l.parts.length > 1 ? l.parts.length : undefined },
    { id: 'crews', name: '班组', badge: l.crews?.length || undefined }, { id: 'method', name: l.chain ? '工序' : '生产方式' }];
  return <section className="line-detail">
    <div className="line-detail-head">
      <div><h2>{l.name} · {l.good}</h2>
        <span className="block-note"><PartsText l={l} /> · {methodText(l)}{l.size < .999 ? ` · 规模 ${decimal(l.size)}：人手、设备与厂房按这类工厂一级的 ${percent(l.size)} 计` : ''}</span></div>
      <div className="seg line-tabs"><Tabs tabs={tabs} value={tab} onChange={onTab} /></div>
    </div>
    {l.project && <ul className="project-list"><ProjectRow p={l.project} canOperate={f.canOperate} /></ul>}
    {tab === 'overview' && <LineOverview f={f} l={l} />}
    {tab === 'products' && <LineProducts key={l.key} f={f} l={l} />}
    {tab === 'crews' && <CrewBlock key={l.key} f={f} l={l} />}
    {tab === 'method' && (l.chain ? <ChainPanel key={l.key} f={f} l={l} /> : <MethodPanel key={l.key} f={f} l={l} />)}
  </section>;
}

/** 新增一条产线（扩建）：选商品与产品，看报价，开工。 */
function NewLine({ f }: { f: FacilityDetail }) {
  const choices = f.options?.expand ?? [];
  const keyOf = (x: { good: string; productId: string }) => `${x.good}:${x.productId}`;
  const [pick, setPick] = useState(() => { const first = choices.find(c => c.quote.ok && !!c.productId) ?? choices[0]; return first ? keyOf(first) : ''; });
  if (f.project) return <section className="line-detail new-line">
    <div className="line-detail-head"><div><h2>扩建进行中</h2><span className="block-note">完工后多一条产线，明天起招工投产。</span></div></div>
    <ul className="project-list"><ProjectRow p={f.project} canOperate={f.canOperate} /></ul>
  </section>;
  if (!choices.length) return null;
  const c = choices.find(x => keyOf(x) === pick) ?? choices[0];
  return <section className="line-detail new-line">
    <div className="line-detail-head"><div><h2>新增一条产线</h2>
      <span className="block-note">现有 {f.lines?.length ?? 0} 条，最多 10 条。新线有自己的班组与班次，生产方式沿用做同种商品的线（没有就用默认方式）。</span></div></div>
    <div className="modal-grid two">
      <section className="panel-block">
        <div className="block-head"><h2>做什么</h2><span className="block-note">一条线只做一种商品</span></div>
        <div className="method-list">{choices.map(x => <button key={keyOf(x)} className={`method ${x === c ? 'active' : ''} ${x.quote.ok ? '' : 'unknown'}`} onClick={() => setPick(keyOf(x))}>
          <b>{x.goodName}线{x.product ? ` · ${x.product}` : ''}</b>
          <span>单班额定 {decimal(x.nominal)} {x.unit}／日 · 人手 {count(x.crew)}{x.quote.ok ? ` · 约 ${pounds(x.quote.total)}` : ` · ${x.quote.reason}`}</span></button>)}</div>
      </section>
      <section className="panel-block">
        <div className="option-head"><b>新增一条{c.goodName}线</b><span>{c.product ? `做 ${c.product}` : `还没有${c.goodName}产品`}</span></div>
        {c.quote.ok ? <>
          <QuoteBar q={c.quote} />
          <StatPairs rows={[['报价', pounds(c.quote.total)], ['当付', pounds(c.quote.dueNow)], ['工期', weeks(c.quote)],
            ['单班额定', `${decimal(c.nominal)} ${c.unit}／日`], ['人手', count(c.crew)], ['规模', c.size < .999 ? `一级的 ${percent(c.size)}` : '一整级']]} />
          {c.quote.orders && <p className="quiet">设备：{c.quote.orders}</p>}
          {!c.productId && <p className="quote-reason">公司还没有{c.goodName}产品：线建成后，要先在公司的产品页设计一个才能开工。</p>}
          {c.ready === false && <p className="quote-reason">{c.product}还在工程验证，{c.readyDate} 可投产。</p>}
          <button className="action" onClick={() => command('project.expand', f.id, c.productId ? { good: c.good, product: c.productId } : { good: c.good })}>开工：新增一条{c.goodName}线</button>
        </> : <QuoteText q={c.quote} />}
      </section>
    </div>
  </section>;
}

// ---- 整处设施 ----

function Equipment({ f }: { f: FacilityDetail }) {
  const methods = (f.lines ?? []).map(l => `${l.name} ${methodText(l)}`);
  if (!f.equipment?.length && !f.method && !methods.length) return null;
  return <section className="panel-block">
    <div className="block-head"><h2><Term name="设备与生产方式">物化在机器里的知识：机床专用度省人手和手艺，精度传递执行质量；蒸汽机功率提高产量，经济性省煤。以 50 为中性。设备属于整处设施，换产时按那条线的份额报废。</Term></h2></div>
    {f.method && <p className="quiet">生产方式：{f.method}</p>}
    {methods.length > 0 && <p className="quiet">生产方式：{methods.join('；')}</p>}
    <ul className="machine-list">{(f.equipment ?? []).map((e, i) => <li key={i}><b>{count(e.units)} {e.good}</b><span>{e.supplier}</span><em>{e.attributes}</em></li>)}</ul>
  </section>;
}

function Books({ f }: { f: FacilityDetail }) {
  return <section className="panel-block"><div className="block-head"><h2>账面</h2></div>
    <StatPairs rows={[['设施账面', pounds(f.book)], ['折旧基数', pounds(f.costBasis)], ['每年折旧', pounds(f.depreciation)]]} /></section>;
}

/** 动力与传送（Processes.md 第 4、9 节）：整座工厂一种，各条线共用；换的时候是整座工厂的工程，各条线一起停工安装。 */
function WorksPanel({ f }: { f: FacilityDetail }) {
  const works = f.works ?? [];
  const [stage, setStage] = useState(works[0]?.stage ?? '');
  const row = works.find(w => w.stage === stage);
  const [way, setWay] = useState('');
  useEffect(() => { setWay(row?.ways?.find(w => w.known)?.id ?? row?.ways?.[0]?.id ?? ''); }, [stage]); // eslint-disable-line react-hooks/exhaustive-deps
  if (!works.length) return null;
  const chosen = row?.ways?.find(w => w.id === way);
  const unit = f.lines?.[0]?.unit ?? '';
  const tone = (a: number, b: number) => (b > a ? 'positive' : b < a ? 'negative' : '');
  return <section className="panel-block works-panel">
    <div className="block-head"><h2><Term name="动力与传送">整座工厂一种，各条线共用。动力决定机器做法能不能用、跑多快（水轮机、电力让要动力的做法更快），蒸汽机每条线一台；传送决定搬运要多少人。工序是产品的设计，在产品的“改工序”里改；换动力、传送是整座工厂的工程，各条线一起停工安装。</Term></h2></div>
    <div className="method-list">{works.map(w => <button key={w.stage} className={`method ${w.stage === stage ? 'active' : ''}`} onClick={() => setStage(w.stage)}>
      <b>{w.name}：{w.wayName}</b><span>{w.effects}</span></button>)}</div>
    {f.worksProject && <ul className="project-list"><ProjectRow p={f.worksProject} canOperate={f.canOperate} /></ul>}
    {f.canOperate && !f.worksProject && !!row?.ways?.length && <>
      <div className="compare-label"><span>换{row.name}</span><span>{row.ways.length} 种做法</span></div>
      <div className="method-list">{row.ways.map(w => <button key={w.id} className={`method ${w.id === way ? 'active' : ''} ${w.known ? '' : 'unknown'}`} onClick={() => setWay(w.id)}>
        <b>{w.name}</b><span>{w.known ? w.effects : `还没有掌握 · ${w.effects}`}</span></button>)}</div>
      {chosen && <div className="way-choice">
        {row.now && chosen.after && <>
          <div className="compare-label"><span>各条线合计（额定）</span><span>现在 → 换上以后</span></div>
          <StatPairs rows={[
            ['日产', `${decimal(row.now.capacity)} → ${decimal(chosen.after.capacity)} ${unit}`, tone(row.now.capacity, chosen.after.capacity)],
            ['人手', `${count(row.now.crew)} → ${count(chosen.after.crew)}`, tone(chosen.after.crew, row.now.crew)]]} />
        </>}
        {!chosen.known ? <p className="quote-reason">还没有掌握：先在公司的“知识与研发”里研发、逆向或取得授权。</p>
          : chosen.quote && (chosen.quote.ok ? <>
            <QuoteBar q={chosen.quote} />
            <StatPairs rows={[['报价', pounds(chosen.quote.total)], ['当付', pounds(chosen.quote.dueNow)], ['全厂停工', weeks(chosen.quote)], ['距离 D', chosen.quote.distance.toFixed(2)]]} />
            {chosen.quote.orders && <p className="quiet">设备：{chosen.quote.orders}</p>}
            <button className="action" onClick={() => command('project.retool', f.id, { method: chosen.id })}>{row.name}换成{chosen.name}（全厂停工安装）</button>
          </> : <QuoteText q={chosen.quote} />)}
      </div>}
    </>}
  </section>;
}

/** 设施：设备、投入品市场、工厂仓库、账面、资源点与封存——整处工厂共有的东西。 */
function WorksFacility({ f }: { f: FacilityDetail }) {
  return <div className="modal-grid two">
    <div className="modal-column">
      <WorksPanel f={f} />
      <Equipment f={f} />
      {!!f.inputs?.length && <section className="panel-block">
        <div className="block-head"><h2><Term name="投入品">各条线的投入品在本集散区的行情：现在送到厂里最便宜的来源（粮食比较粮仓、经销商、各地块农户与港口进口，含车马费），区内现货，以及上周从哪里进的货。</Term></h2></div>
        <div className="table machines"><span className="th">投入品</span><span className="th">现在最便宜（到厂）</span><span className="th">区内现货</span><span className="th">上周进货</span>
          {f.inputs.map(i => <Fragment key={i.good}><span>{i.good}<em className="detail-note">{(f.lines ?? []).filter(l => l.inputs?.some(x => x.good === i.good)).map(l => l.name).join('、')}</em></span>
            <span>{price(i.price ?? i.outside)}／{i.unit}<em className="detail-note">{i.source ?? '模型外'}</em></span><span>{count(i.local)} {i.unit}</span>
            <span>{i.lastWeek?.length ? i.lastWeek.map(b => `${b.source} ${decimal(b.quantity)} ${b.unit}（${price(b.cost / Math.max(b.quantity, 1e-9))}）`).join('；') : '—'}</span></Fragment>)}
        </div>
      </section>}
      {!!f.warehouse?.length && <section className="panel-block">
        <div className="block-head"><h2><Term name="工厂仓库">产出先进工厂仓库，再按缺口分给本地直销仓和各经销商。仓库最多存额定产量的 30 天，满了就减产。</Term></h2></div>
        {f.warehouse.map(w => <Meter key={w.product} label={w.product} value={w.cap > 0 ? w.quantity / w.cap : 0} text={`${count(w.quantity)} / ${count(w.cap)} ${w.unit}`} tone={w.cap > 0 && w.quantity / w.cap > .9 ? 'warn' : ''} />)}
      </section>}
    </div>
    <div className="modal-column">
      <Books f={f} />
      {f.resourcePoint && <section className="panel-block"><div className="block-head"><h2>{f.resourceBlocks !== undefined
          ? <Term name="矿藏与矿权">按块开采：只采本公司租下、生效的矿区，块号小的先采；每吨的矿租记进出矿的成本。租下的矿区采完或失效，矿就停；在公司面板的“矿权”页签续租，或在地块档案再租一块。</Term> : '资源点'}</h2></div>
        <StatPairs rows={f.resourceBlocks !== undefined
          ? [['矿藏', f.resourcePoint], ['租下的矿区', f.resourceBlocks ? `${f.resourceBlocks} 块，还剩 ${tons(f.resourceLeft ?? 0)}` : '没有生效的矿区'],
             ['整处矿藏还剩', tons(f.resourceDeposit ?? 0)], ['每吨矿租', f.royaltyPerTon !== undefined ? price(f.royaltyPerTon) : '—']]
          : [['名称', f.resourcePoint], ['剩余', f.resourceDepletes ? count(f.resourceLeft ?? 0) : '不会耗尽']]} /></section>}
      {f.canOperate && f.options?.canIdle && <section className="panel-block">
        <div className="option-card">
          <div className="option-head"><b>{f.idle ? '重新启用' : '封存整处工厂'}</b><span>{f.idle ? '各条线重新招工开工。' : '遣散各条线的班组，停产，不付维护费；账面照常折旧。'}</span></div>
          <button className="action quiet-action" onClick={() => command('facility.set_idle', f.id, { idle: f.idle ? '0' : '1' })}>{f.idle ? '启用' : '封存'}</button>
        </div>
      </section>}
    </div>
  </div>;
}

/** 服务设施的工程：扩建一级。 */
function ServiceEngineering({ f }: { f: FacilityDetail }) {
  if (f.project) return <section className="panel-block"><div className="block-head"><h2>工程</h2></div><ul className="project-list"><ProjectRow p={f.project} canOperate={f.canOperate} /></ul></section>;
  const q = f.options?.expandSize;
  if (!q) return null;
  return <section className="panel-block engineering">
    <div className="block-head"><h2><Term name="工程">单体、长工期、按节点付款、结果有方差：开工时掷出超支（−5%～+25%）与延误（0～6 周）。设备向机器厂按单订购，先付订金、货到付清；模型外的设备要 8 周。</Term></h2></div>
    <div className="option-card">
      <div className="option-head"><b>扩建一级</b><span>规模 {f.size} → {f.size + 1}</span></div>
      {q.ok ? <><QuoteBar q={q} /><StatPairs rows={[['报价', pounds(q.total)], ['当付', pounds(q.dueNow)], ['工期', weeks(q)]]} />
        {q.orders && <p className="quiet">设备：{q.orders}</p>}
        <button className="action" onClick={() => command('project.expand', f.id)}>开工扩建</button></> : <QuoteText q={q} />}
    </div>
  </section>;
}

/** 基地：登记在这里的车船，去向与台数；调派和订购在公司的车队页签。 */
function BaseFleets({ f }: { f: FacilityDetail }) {
  const openModal = useUIStore(s => s.openModal);
  return <section className="panel-block">
    <div className="block-head"><h2><Term name="登记的车船">{`这处设施是${(f.baseGoods ?? []).join('、')}的基地：乘员在这里雇、燃料在这个集散区买。跑线路的车船要以线路一端为基地；马车站的车只在本站地块送货，站越大，地块的配送覆盖越高、送货越便宜。`}</Term></h2></div>
    {f.fleets?.length ? <ul className="offer-list compact">{f.fleets.map(x => <li key={x.id}>
      <span><b>{x.name}</b><em>{x.goodName} · {x.assignment}{!x.running && (x.line || x.market) ? '（停开）' : ''}</em></span>
      <span className="stock">{count(x.units)} {x.unit}<em>账面 {pounds(x.book)}</em></span></li>)}</ul> : <p className="quiet">还没有车船登记在这里。</p>}
    <button className="link" onClick={() => openModal({ kind: 'company', id: f.companyId, tab: 'transport' })}>到公司的车队页签调派、订购、定运价</button>
  </section>;
}

function BuildingBody({ f }: { f: FacilityDetail }) {
  return <div className="modal-grid two">
    <div className="modal-column">
      <section className="panel-block">
        <div className="block-head"><h2><Term name="按期产能">服务设施每天开放座位、床位、车次或印数；当天卖不掉就作废。</Term></h2></div>
        {f.capacity?.length ? <StatPairs rows={f.capacity.map(c => [c.good, `${decimal(c.perDay)} ${c.unit}／日`])} /> : <p className="quiet">这类设施没有按期产能。</p>}
        {!!f.coverage?.length && <StatPairs rows={f.coverage.map(c => [`覆盖（${c.access}）`, percent(c.share)])} />}
        {f.freightCoverage !== undefined && <StatPairs rows={[['本地块配送覆盖', percent(f.freightCoverage)], ['本地块送货费率', `×${(f.deliveryFare ?? 1).toFixed(2)}`]]} />}
      </section>
      {f.fleets && <BaseFleets f={f} />}
      <AllocationPanel f={f} />
      {f.granary && <GranaryPanel f={f} />}
      <DepotPanel f={f} />
      <HousePanel f={f} />
      <Equipment f={f} />
    </div>
    <div className="modal-column">
      <ServiceEngineering f={f} />
      <Books f={f} />
    </div>
  </div>;
}

type FacilityTab = 'lines' | 'facility';

function WorksBody({ f, line, tab, tabState }: { f: FacilityDetail; line?: number; tab: FacilityTab; tabState?: LineTab }) {
  const lines = f.lines ?? [];
  const [pick, setPick] = useState<Pick>(() => line !== undefined && lines.some(l => l.id === line) ? line : lines[0]?.id ?? 'new');
  const [lineTab, setLineTab] = useState<LineTab>(tabState ?? 'overview');
  const chosen = typeof pick === 'number' ? lines.find(l => l.id === pick) : undefined;
  // A line that is gone (or the finished expansion) falls back to the first line.
  useEffect(() => {
    if (pick === 'new' ? !f.project && !(f.canOperate && f.options?.expand.length) : !chosen) setPick(lines[0]?.id ?? 'new');
  }, [pick, chosen, f.project, f.canOperate, f.options, lines]);
  if (tab === 'facility') return <WorksFacility f={f} />;
  return <>
    <LineStrip f={f} pick={pick} onPick={setPick} />
    {pick === 'new' ? <NewLine f={f} /> : chosen && <LineDetail f={f} l={chosen} tab={lineTab} onTab={setLineTab} />}
  </>;
}

export function FacilityModal({ world, id, line, tab, onClose }: { world: World; id: string; line?: number; tab?: string; onClose: () => void }) {
  const f = world.facility?.id === id ? world.facility : undefined;
  if (!f) return <Modal eyebrow="设施" title="正在读取设施档案…" onClose={onClose}><p className="quiet">正在读取设施档案…</p></Modal>;
  return <FacilityView f={f} line={line} tab={tab} onClose={onClose} />;
}

function FacilityView({ f, line, tab, onClose }: { f: FacilityDetail; line?: number; tab?: string; onClose: () => void }) {
  const openModal = useUIStore(s => s.openModal);
  const [view, setView] = useState<FacilityTab>('lines');
  const lines = f.lines ?? [];
  const works = lines.length > 0;
  const goods = f.production?.goods ?? [];
  const tabs: { id: FacilityTab; name: string; badge?: React.ReactNode }[] = [{ id: 'lines', name: '产线', badge: lines.length }, { id: 'facility', name: '设施' }];
  return <Modal className="facility-modal" onClose={onClose}
    eyebrow={<>{works ? `工厂 · ${f.type} · ${lines.length} 条产线` : `建筑 · ${f.type}${f.size > 1 ? ` · 规模 ${f.size}` : ''}`}{f.business ? ` · ${f.business}` : ''}</>}
    title={f.name}
    meta={<><button className="link" onClick={() => openModal({ kind: 'company', id: f.companyId })}>{f.company}</button><em className={f.mine ? 'mine' : ''}>{f.group || '无控制集团'}</em>
      <span>{f.plot} · {f.market}{f.country ? ` · ${f.country}` : ''}</span><StatusBadge status={f.status} tone={statusTone(f.status)} />
      {!f.canOperate && <span className="readonly">只读：你的集团不经营这家公司</span>}</>}
    figures={<>
      {goods.map(g => <Cell key={g.good} label={`${g.good}·昨日`} value={decimal(g.produced)} note={`额定 ${decimal(g.nominal)} ${g.unit}`} />)}
      {f.production && <Cell label="人手" value={`${count(f.production.heads)} / ${count(f.production.required)}`} tone={f.production.heads < f.production.required ? 'negative' : ''}
        note={goods.some(g => g.nominal > 0) ? `熟练度 ${percent(f.production.effective)}` : '还没有开工'} />}
      <Cell label="设施账面" value={shortPounds(f.book)} note={`年折旧 ${shortPounds(f.depreciation)}`} />
    </>}
    actions={<button className="link" onClick={() => { focusFacility(f.id); onClose(); }}>在地图上定位</button>}
    tabs={works ? <Tabs tabs={tabs} value={view} onChange={setView} /> : undefined}>
    {works ? <WorksBody f={f} line={line} tab={view} tabState={tab === 'products' || tab === 'crews' || tab === 'method' ? tab : undefined} /> : <BuildingBody f={f} />}
  </Modal>;
}
