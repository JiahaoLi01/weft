import { Fragment, useMemo, useState } from 'react';
import { Term } from '@/components/Modal';
import { Select } from '@/components/Select';
import { UtilBars } from '@/components/charts';
import { command, count, decimal, percent, pounds, price } from '@/lib/format';
import { useQuote } from './ConstructModal';
import type { FleetView, ModelView, ServiceView, TransportBase, TransportView } from '@/types';

// 运输运营（Transport.md §16）：公司的车队、在跑的服务与运价、向制造商或模型外订购车船。

/** Room by cargo class: tons, and seats for passengers (a coach's room is counted in seats). */
function roomText(v: { room?: { class: string; tons: number; seats?: number }[]; payload: number }) {
  return v.room?.length ? v.room.map(r => r.seats !== undefined ? `${r.class} ${count(r.seats)} 座` : `${r.class} ${decimal(r.tons)} 吨`).join('、') : `${decimal(v.payload)} 吨`;
}

/** One group: model, units, where it runs; for the operator: send it elsewhere, lay it up, sell it. */
function FleetRow({ f, canOperate }: { f: FleetView; canOperate: boolean }) {
  const [target, setTarget] = useState('');
  const [units, setUnits] = useState(String(f.units));
  const moving = Math.max(1, Math.min(f.units, Math.round(Number(units) || f.units)));
  const options = [
    { value: '', label: '派往…' },
    ...(f.targets ?? []).map(t => ({ value: t.line ? `line:${t.line}` : `market:${t.market}`, label: t.name })),
    ...(f.line || f.market ? [{ value: 'layup', label: '停泊在基地' }] : []),
  ];
  const send = () => {
    const [kind, id] = target === 'layup' ? ['', ''] : target.split(':');
    command('fleet.assign', f.id, { units: String(moving), line: kind === 'line' ? id : '', market: kind === 'market' ? id : '' });
    setTarget('');
  };
  // 耐用品：the life used is its age and the years lost to upkeep not done.
  const used = f.usedYears ?? f.ageYears;
  const worn = f.lifeYears > 0 ? used / f.lifeYears : 0;
  const short = f.billDue !== undefined && f.billPaid !== undefined && f.billPaid < f.billDue;
  return <li>
    <div className="fleet-main">
      <span className="fleet-name"><b>{f.name}</b><em>{f.goodName} · {f.maker}</em></span>
      <span className="fleet-units"><b>{count(f.units)}</b> {f.unit}<em>{f.base}</em></span>
      <span className={`fleet-where ${f.running ? '' : 'idle'}`}>{f.assignment}{!f.running && (f.line || f.market) ? '（开不了）' : ''}</span>
    </div>
    <span className="fleet-design">{f.design}</span>
    <span className="fleet-spec">每{f.unit}载重 {roomText(f)} · {count(f.kmPerDay)} 公里／日 · 乘员 {f.crew}
      {f.fuel.length ? ` · ${f.fuel.map(x => `${x.good} ${decimal(x.tonsPerDay)} 吨／日`).join('、')}` : ''}</span>
    <span className={`fleet-age ${worn > .85 ? 'warn' : ''}`}>车龄 {f.ageYears.toFixed(1)} 年{f.lostYears ? `，欠维护折寿 ${f.lostYears.toFixed(1)} 年` : ''} · 寿命 {f.lifeYears.toFixed(0)} 年，还剩 {Math.max(0, f.lifeYears - used).toFixed(1)} 年 · 账面 {pounds(f.book)}</span>
    {f.keeper && <span className={`fleet-keep ${f.unkept || short ? 'warn' : ''}`}>维护：{f.keeper}
      {f.upkeepYear ? ` · 每年约 ${pounds(f.upkeepYear)}，每 ${f.settleDays ?? 28} 天结一次账` : ''}
      {f.unkept ? ` · 没人修，每天按 ${f.missedWear ?? 4} 天磨损` : ''}
      {short ? ` · 上期只付了 ${pounds(f.billPaid ?? 0)}／${pounds(f.billDue ?? 0)}，没付的那部分天数按 ${f.missedWear ?? 4} 倍磨损` : ''}</span>}
    {canOperate && f.noTargets && <span className="quote-reason">{f.noTargets}</span>}
    {canOperate && <span className="fleet-actions">
      <Select value={target} onChange={setTarget} ariaLabel="去向" options={options} minWidth={190} />
      {f.units > 1 && <label>台数 <input value={units} onChange={e => setUnits(e.target.value)} inputMode="numeric" /></label>}
      <button className="link" disabled={!target} onClick={send}>调派 {count(moving)} {f.unit}</button>
      <button className="link" onClick={() => command('fleet.sell', f.id, { units: String(moving) })}>出售 {count(moving)} {f.unit}（得 {pounds(f.book * moving / Math.max(1, f.units) * .6)}）</button>
    </span>}
  </li>;
}

/** One service: fare (× the base), last week's room, tons, takings and running costs; for the operator, the fare. */
function ServiceRow({ s, company, canOperate }: { s: ServiceView; company: string; canOperate: boolean }) {
  const [value, setValue] = useState('');
  const set = (multiple: number) => command('fleet.set_rate', company, { line: s.line, market: s.market, permille: String(Math.round(multiple * 1000)) });
  const setPax = (multiple: number) => command('fleet.set_rate', company, { line: s.line, market: '', permille: String(Math.round(multiple * 1000)), passenger: '1' });
  const margin = (s.revenue ?? 0) - (s.costs ?? 0);
  const seatUtil = (s.seatsOffered ?? 0) > 0 ? (s.seatsTaken ?? 0) / (s.seatsOffered ?? 1) : undefined;
  return <li>
    <div className="fleet-main">
      <span className="fleet-name"><b>{s.name}</b>
        <em>{s.line ? `${count(s.vehicles ?? 0)} 台 · 在途 ${s.transit ?? 0} 天 · ${(s.every ?? 1) > 1 ? `每 ${s.every} 天一班` : '每天一班'} · 每班 ${decimal(s.room ?? 0)} 吨${s.rivals && !s.rivalList?.length ? ` · 同线另有 ${s.rivals} 家` : ''}`
          : `${count(s.vehicles ?? 0)} 辆 · 每天 ${decimal(s.tonsPerDay ?? 0)} 吨`}</em></span>
      <span className="fleet-units">运价 <b>×{s.fare.toFixed(2)}</b>{s.basePerTon ? <em>每吨 {price(s.basePerTon * s.fare)}</em> : null}</span>
    </div>
    <span className="service-util">上周 {decimal(s.carried ?? 0)}／{decimal(s.offered ?? 0)} 吨 · 利用率 {percent(s.util ?? 0)}<UtilBars ratio={s.util ?? 0} /></span>
    <span className={`service-money ${margin < 0 ? 'negative' : ''}`}>运费收入 {pounds(s.revenue ?? 0)}{s.paxRevenue ? `（客票 ${pounds(s.paxRevenue)}）` : ''} · 运营成本 {pounds(s.costs ?? 0)} · 毛利 {pounds(margin)}</span>
    {s.paxFare !== undefined && <span className="service-pax">
      <span>客运 · {s.seats ? `每班 ${count(s.seats)} 座${s.sharedHold ? '，另可占舱位' : ''}` : '旅客与货物共用舱位'} · 车上 {s.paxDays ?? 1} 天 · 上周旅客 {count(s.passengers ?? 0)} 人次{seatUtil !== undefined ? ` · 上座率 ${percent(seatUtil)}` : ''}</span>
      <span>票价 <b>×{s.paxFare.toFixed(2)}</b><em>每人单程 {price((s.paxBase ?? 0) * s.paxFare)}{s.className && s.classShare ? ` · ${s.className} ${price((s.paxBase ?? 0) * s.paxFare * s.classShare)}` : ''}</em></span>
      {canOperate && <span className="price-actions">
        <button className="link" onClick={() => setPax((s.paxFare ?? 1) * .95)}>票价降 5%</button>
        <button className="link" onClick={() => setPax((s.paxFare ?? 1) * 1.05)}>票价提 5%</button>
      </span>}
    </span>}
    {!!s.rivalList?.length && <div className="rival-list">
      <span className="rival-head">同线别家</span>
      {s.rivalList.map((r, i) => <div key={i} className="rival-row">
        <span><b>{r.name}</b><em>{r.vehicleModels || `${count(r.vehicles)} 台`} · {r.every > 1 ? `每 ${r.every} 天一班` : '每天一班'} · 每班 {decimal(r.room)} 吨{r.seats ? `、${count(r.seats)} 座` : ''} · 在途 {r.transit} 天</em></span>
        <span>运价 每吨 <b>{price(r.rate)}</b>{r.rateX !== undefined ? `（×${r.rateX.toFixed(2)}）` : ''}
          {r.paxFare !== undefined ? <> · 客票 <b>{price(r.paxFare)}</b>{r.paxX !== undefined ? `（×${r.paxX.toFixed(2)}）` : ''}{r.className ? ` · ${r.className} ${price(r.classFare ?? 0)}` : ''}</> : null}</span>
        {(r.offered !== undefined || r.seatsOffered !== undefined) && <span className="rival-week">上周
          {r.offered !== undefined ? ` 货运 ${decimal(r.carried ?? 0)}／${decimal(r.offered)} 吨` : ''}
          {r.seatsOffered ? ` · 旅客 ${count(r.passengers ?? 0)} 人次，上座率 ${percent((r.seatsTaken ?? 0) / r.seatsOffered)}` : ''}</span>}
      </div>)}
    </div>}
    {canOperate && <span className="price-actions">
      <button className="link" onClick={() => set(s.fare * .95)}>降 5%</button>
      <button className="link" onClick={() => set(s.fare * 1.05)}>提 5%</button>
      <label>倍数 <input value={value} onChange={e => setValue(e.target.value)} inputMode="decimal" /></label>
      <button className="link" disabled={!(Number(value) >= .1 && Number(value) <= 5)} onClick={() => { set(Number(value)); setValue(''); }}>定运价</button>
    </span>}
  </li>;
}

/** 订购：a vehicle good for one base, of a maker's model or the neutral design outside the model. */
function OrderForm({ company, base, good, day }: { company: string; base: TransportBase; good: TransportBase['goods'][number]; day?: number }) {
  const [model, setModel] = useState('');
  const [units, setUnits] = useState('1');
  const n = Math.max(1, Math.min(500, Math.round(Number(units) || 1)));
  const request = useMemo(() => ({ kind: 'fleet', company, base: base.id, good: good.id, units: String(n), product: model }), [company, base.id, good.id, n, model]);
  const reply = useQuote(request, day);
  const q = reply?.quote;
  const chosen = good.models.find(m => m.id === model);
  // 选型比较：the operators' AI orders the cheapest per ton-km among the models that keep a service's speed and cargo.
  const rows: ModelView[] = [...(good.outside ? [good.outside] : []), ...good.models];
  const quoted = rows.filter(m => m.ok && (m.perKTonKm ?? 0) > 0);
  const cheapest = quoted.length ? Math.min(...quoted.map(m => m.perKTonKm ?? Infinity)) : 0;
  return <div className="order-good">
    <span className="order-head"><b>{good.name}</b><em>{chosen ? chosen.design : good.outsideDesign}</em></span>
    {rows.length > 1 && <div className="table model-compare">
      <span className="th">型号</span><span className="th">公里／日</span><span className="th">载重</span><span className="th">到岸价</span>
      <span className="th"><Term name="每千吨公里">满载时每千吨公里的成本：购价按寿命摊销加利息、乘员工资、维护（按耐用）和燃料（按本地的模型外价格）。运营商的 AI 在速度不低于现有车船九成、货类相同的型号里挑最便宜的。</Term></span>
      {rows.map(m => { const cls = `${m.id === model ? 'chosen' : ''} ${m.ok && m.perKTonKm === cheapest ? 'best' : ''}`; return <Fragment key={m.id || 'outside'}>
        <span className={cls}><button className="link" onClick={() => setModel(m.id)}>{m.name}</button><em>{m.maker}{m.durability !== 50 ? ` · 耐用 ${m.durability}` : ''}</em></span>
        <span className={cls}>{count(m.kmPerDay)}</span>
        <span className={cls}>{roomText(m)}</span>
        <span className={cls}>{m.ok ? pounds(m.landed ?? m.price) : <em className="quote-reason">{m.reason}</em>}</span>
        <span className={cls}>{m.ok && m.perKTonKm ? price(m.perKTonKm) : '—'}</span>
      </Fragment>; })}
    </div>}
    <div className="inline-form">
      <Select value={model} onChange={setModel} ariaLabel="型号" minWidth={230} options={[
        { value: '', label: '模型外 · 默认设计' }, ...good.models.map(m => ({ value: m.id, label: `${m.name} · ${m.maker} · ${price(m.price)}` }))]} />
      <label>数量 <input value={units} onChange={e => setUnits(e.target.value)} inputMode="numeric" /> {good.unit}</label>
      {q && (q.ok ? <span className="quote">约 {pounds(q.total)} · 当付 {pounds(q.dueNow)} · 约 {Math.max(1, Math.round(q.days / 7))} 周到</span>
        : <span className="quote-reason">{q.reason}</span>)}
      <button className="action" disabled={!q?.ok || !reply?.canOperate} onClick={() => command('fleet.order', company, { good: good.id, units: String(n), base: base.id, product: model })}>订购</button>
    </div>
  </div>;
}

export function TransportPanel({ company, t, canOperate, day }: { company: string; t: TransportView; canOperate: boolean; day?: number }) {
  return <div className="modal-grid two">
    <div className="modal-column">
      <section className="panel-block">
        <div className="block-head"><h2><Term name="车队">车船按“型号 × 基地 × 去向”合并记账。跑线路的车船要以线路一端为基地；马车站的车只在本站所在的地块送货。跑着的车船付全额乘员、燃料与维护，停泊只付一成乘员、一半维护；出售到模型外按账面的六成。</Term></h2>
          <span className="block-note">账面 {pounds(t.book)} · 固定开支 {pounds(t.fixedWeekly)}／周</span></div>
        {t.fleets.length ? <ul className="fleet-rows">{t.fleets.map(f => <FleetRow key={f.id} f={f} canOperate={canOperate} />)}</ul> : <p className="quiet">还没有车船。</p>}
      </section>
    </div>
    <div className="modal-column">
      <section className="panel-block">
        <div className="block-head"><h2><Term name="服务与运价">运价是线路基准运价的倍数。托运人按“运价 + 时间价值 × (等船 + 在途天数)”挑承运人：茶叶、鸦片肯为快船多付，煤和粮食挑便宜的慢船。集散区内按地块结算：马车站按运价从低到高分吨位，比模型外车夫贵就分不到货。票价另算：旅客坐列车的客车厢，船上与货物共用舱位；一次往返要去程、回程都有座才卖。</Term></h2></div>
        {t.services.length ? <ul className="fleet-rows">{t.services.map(s => <ServiceRow key={s.key} s={s} company={company} canOperate={canOperate} />)}</ul> : <p className="quiet">没有在跑的服务。</p>}
      </section>
      {canOperate && !!t.bases.length && <section className="panel-block">
        <div className="block-head"><h2><Term name="订购车船">按单订购，到货后停泊在基地，再派出去。模型外的默认设计全款预付、约 8 周到；向制造商订先付三成订金，按它的排产交货。船和列车沿自己能走的线路开来，不付运费；过不去的（跨洋的列车、运往印度的内河汽船）按空车重量当货运，运费由制造商付、算进到岸价。</Term></h2></div>
        {t.bases.map(b => <div key={b.id} className="order-base">
          <h3>{b.name}<em> · {b.type} · {b.market}</em></h3>
          {b.goods.map(g => <OrderForm key={g.id} company={company} base={b} good={g} day={day} />)}
        </div>)}
      </section>}
    </div>
  </div>;
}
