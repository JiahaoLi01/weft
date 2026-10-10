import { useState } from 'react';
import { Term } from '@/components/Modal';
import { Select } from '@/components/Select';
import { Meter } from '@/components/charts';
import { command, count, decimal, percent, price } from '@/lib/format';
import { tons } from './Mining';
import type { DepotStore, FacilityDetail, RentalRow } from '@/types';

// 集散仓（Trade.md 第 2 节）：仓容、码头与装卸；班次（装卸、脚夫与开支）；上周的进出、压港与损耗；仓里每批货的数量、产地、
// 是否还在码头上、这里每月的损耗。

/** Tons, with a decimal below ten (a few tons spoiled, a little left on the dock). */
const t = (value: number) => value > 0 && value < 10 ? `${decimal(value)} 吨` : tons(value);

/** 装卸：the porters' shift for all the company's depots in the 集散区, each shift's 装卸 and what it costs a week. */
function Gate({ f, v }: { f: FacilityDetail; v: DepotStore }) {
  const single = v.shifts.find(s => s.id === 'single');
  const cur = (id: string) => v.shift === id ? 'cur' : '';
  return <div className="depot-gate">
    <Meter label="仓容" value={v.capacity > 0 ? (v.held + v.dock) / v.capacity : 0} text={`${t(v.held + v.dock)} / ${t(v.capacity)}`}
      tone={v.capacity > 0 && (v.held + v.dock) / v.capacity > .9 ? 'warn' : ''} />
    <Meter label="昨天装卸" value={v.throughput > 0 ? v.lastDay / v.throughput : 0} text={`${t(v.lastDay)} / ${t(v.throughput)}`}
      tone={v.throughput > 0 && v.lastDay / v.throughput > .95 ? 'warn' : ''} />
    {v.dock > 0 && <p className="depot-dock">码头上压着 {t(v.dock)}：卸进仓之前不能卖。</p>}
    <div className="shift-head"><b><Term name="装卸">每天出仓加进仓合计有上限：规模 × 每级每天的装卸 × 班次。半班省一半脚夫，装卸也少一半；双班多装卸八成，脚夫加倍还要加点工钱。AI 一周里有三天收市时码头上还压着货就开双班，一周的进出不到单班的三分之一就回到单班。这家公司在本集散区的几座集散仓一起算。</Term></b>
      {f.canOperate ? <span className="seg small" role="group" aria-label="班次">{v.shifts.map(s => <button key={s.id} className={v.shift === s.id ? 'active' : ''} aria-pressed={v.shift === s.id}
        disabled={v.shift === s.id} onClick={() => command('depot.gate', f.companyId, { market: f.marketId, shift: s.id })}>{s.name}</button>)}</span>
        : <span>{v.shifts.find(s => s.id === v.shift)?.name}</span>}</div>
    <div className="shift-grid" style={{ gridTemplateColumns: `auto repeat(${v.shifts.length}, minmax(0, 1fr))` }}>
      <span />{v.shifts.map(s => <span key={s.id} className={cur(s.id)}>{s.name}</span>)}
      <span>装卸／日</span>{v.shifts.map(s => <span key={s.id} className={cur(s.id)}>{t(s.throughput)}</span>)}
      <span>脚夫</span>{v.shifts.map(s => <span key={s.id} className={cur(s.id)}>{count(s.porters)} 人</span>)}
      <span>工钱／周</span>{v.shifts.map(s => <span key={s.id} className={cur(s.id)}>{price(s.wagesPerWeek)}</span>)}
      <span>维护／周</span>{v.shifts.map(s => <span key={s.id} className={cur(s.id)}>{price(s.upkeepPerWeek)}</span>)}
    </div>
    <p className="deposit-terms">上周进出 {t(v.lastWeek)}{single && single.throughput > 0 ? `（单班一周能力的 ${percent(v.lastWeek / (single.throughput * 7))}）` : ''}；
      {v.dockDays > 0 ? `${v.dockDays} 天收市时码头上还压着货` : '码头没有压货'}；损耗 {t(v.spoiled)}</p>
  </div>;
}

/** One rental: who, how much, the rent and where it stands; answer or take back a proposal. */
function Rental({ r }: { r: RentalRow }) {
  const state = r.status === 'proposed' ? `待答复，还有 ${r.answerDays ?? 0} 天` : r.status === 'active' ? `到 ${r.until}，已付 ${r.paidMonths}／${r.months} 个月` : r.statusName;
  return <li className={r.faultDays !== undefined ? 'rental-row fault' : 'rental-row'}>
    <span><b>{r.role === 'owner' ? `租给 ${r.otherName}` : `向 ${r.otherName} 租`}</b><em>{count(r.tons)} 吨 · {r.months} 个月 · 保证金各 {percent(r.deposit)}</em></span>
    <span className="stock">{price(r.rent)}／月<em>{state}</em></span>
    {r.faultDays !== undefined && <span className="line-limit">欠租：{r.faultDays} 天内补上，否则取消，保证金归仓主</span>}
    {r.canAnswer && <span className="price-actions"><button className="link" onClick={() => command('rental.accept', r.id)}>接受</button>
      <button className="link" onClick={() => command('rental.decline', r.id)}>拒绝</button></span>}
    {r.canWithdraw && <span className="price-actions"><button className="link" onClick={() => command('rental.withdraw', r.id)}>撤回</button></span>}
  </li>;
}

/** 出租：what is let here and rented, proposed or lately ended; a form to rent room in it for one of the viewer's companies. */
function Rentals({ f, v }: { f: FacilityDetail; v: DepotStore }) {
  const [tenant, setTenant] = useState(v.renters[0]?.id ?? '');
  const [tons, setTons] = useState('500');
  const [months, setMonths] = useState('6');
  const n = Number(tons), m = Number(months);
  const fair = v.costPerTonMonth * (Number.isFinite(n) ? n : 0) * 1.2;
  const [rent, setRent] = useState('');
  const r = rent.trim() ? Number(rent) : fair;
  const ok = !!tenant && Number.isFinite(n) && n >= 1 && n <= v.free && Number.isFinite(m) && m >= 1 && m <= 60 && Number.isFinite(r) && r > 0;
  return <>
    {!!v.rentals.length && <ul className="offer-list compact">{v.rentals.map(r => <Rental key={r.id} r={r} />)}</ul>}
    {!v.rented && <p className="deposit-terms">空余 {t(v.free)} 可以出租，成本约每吨每月 {price(v.costPerTonMonth)}（脚夫与维护）。</p>}
    {!v.rented && !!v.renters.length && v.free >= 1 && <div className="inline-form depot-rent">
      <Select value={tenant} onChange={setTenant} ariaLabel="租用的公司" minWidth={150} options={v.renters.map(x => ({ value: x.id, label: x.name }))} />
      <label>吨<input value={tons} onChange={e => setTons(e.target.value.replace(/[^0-9]/g, ''))} inputMode="numeric" style={{ width: 56 }} /></label>
      <label>月<input value={months} onChange={e => setMonths(e.target.value.replace(/[^0-9]/g, ''))} inputMode="numeric" style={{ width: 36 }} /></label>
      <label>月租（£，空着按成本加两成 {price(fair)}）<input value={rent} onChange={e => setRent(e.target.value.replace(/[^0-9.]/g, ''))} inputMode="decimal" style={{ width: 64 }} /></label>
      <button className="link" disabled={!ok} onClick={() => command('rental.propose', tenant, { role: 'tenant', other: f.companyId, market: f.marketId, tons: String(n), months: String(m), rent: r.toFixed(2) })}>租用</button>
    </div>}
  </>;
}

export function DepotPanel({ f }: { f: FacilityDetail }) {
  const stock = f.depot;
  if (!stock) return null;
  const v = f.store;
  return <section className="panel-block">
    <div className="block-head"><h2><Term name="集散仓">货归集散仓：经销商从外地订来的货卸在这里，再卖给本集散区的家庭和工厂。仓容按重量算，什么货都能存。先出后进：白天卖出去的货先出仓，收市后到岸的货才卸进仓；装卸用完了或仓满了，剩下的停在码头上，码头上的货不能卖，照样损耗。补货按预测销量订舱，仓里的和路上的一起算，不超过仓容。存货按商品的月损耗率损耗，湿热的地方多，寒冷的地方少。</Term></h2>
      {v && <span className="block-note">存 {t(v.held + v.dock)}／{t(v.capacity)}</span>}</div>
    {v && <Gate f={f} v={v} />}
    {v && <Rentals f={f} v={v} />}
    {stock.length ? <ul className="offer-list compact">{stock.map((s, i) => <li key={i} className={s.dock ? 'on-dock' : ''}>
      <span><b>{s.product}</b><em>{s.maker}</em></span>
      <span className="stock">{count(s.quantity)} {s.unit}<em>产地 {s.origin}{s.dock ? ' · 码头上' : ''}{s.spoilage ? ` · 每月损耗 ${percent(s.spoilage)}` : ''}</em></span>
    </li>)}</ul> : <p className="quiet">仓里没有货。</p>}
  </section>;
}
