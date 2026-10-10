import { useState } from 'react';
import { Term } from '@/components/Modal';
import { StatPairs } from '@/components/charts';
import { command, count, percent, pounds, price } from '@/lib/format';
import { tons } from './Mining';
import type { FacilityDetail, GranaryGrain, GranaryView } from '@/types';

// 粮仓（Agriculture.md 第 4 节）：容量与存粮；仓门的班次（吞吐与脚夫）和常平的底仓；歉收与平粜的提醒；每种粮的经营（常平、收粮、
// 存粮、卖粮），今天的收购价与卖价，下一两季的年景，全年的价格区间，现在的行情，上周的进出，周边农户看到的到手价。

const ways = [
  { id: 'auto', name: '常平' },
  { id: 'buy', name: '收粮' },
  { id: 'hold', name: '存粮' },
  { id: 'sell', name: '卖粮' },
] as const;

/** "2.85" → a number; empty is none (the market's price, or the default amount); bad input is NaN. */
const parse = (text: string) => { if (!text.trim()) return undefined; const v = Number(text); return Number.isFinite(v) && v >= 0 ? v : NaN; };

function setWay(f: FacilityDetail, g: GranaryGrain, mode: string, limit?: number, target?: number) {
  command('granary.set', f.companyId, { market: f.marketId, crop: g.crop, mode,
    ...(limit ? { price: limit.toFixed(4) } : {}), ...(target !== undefined ? { target: String(Math.round(target)) } : {}) });
}

/** 收粮、卖粮：a price of one's own (empty: the market's) and how far to go (empty: a full store, or an empty one). */
function WayForm({ f, g }: { f: FacilityDetail; g: GranaryGrain }) {
  const buying = g.mode === 'buy';
  const [limit, setLimit] = useState(g.limit > 0 ? g.limit.toFixed(2) : '');
  const [target, setTarget] = useState(g.target >= 0 ? String(Math.round(g.target)) : '');
  const l = parse(limit), t = parse(target);
  const ok = !Number.isNaN(l) && !Number.isNaN(t);
  return <div className="inline-form granary-form">
    <label>{buying ? '出价' : '要价'}（£／吨，空着随市价）<input value={limit} onChange={e => setLimit(e.target.value.replace(/[^0-9.]/g, ''))} inputMode="decimal" style={{ width: 64 }} /></label>
    <label>{buying ? '收到' : '卖到剩'}（吨，空着{buying ? '收满仓' : '卖空'}）<input value={target} onChange={e => setTarget(e.target.value.replace(/[^0-9]/g, ''))} inputMode="numeric" style={{ width: 64 }} /></label>
    <button className="link" disabled={!ok} onClick={() => setWay(f, g, g.mode, l, t)}>改</button>
  </div>;
}

/** What the store does with the grain today, in a sentence. */
function doing(g: GranaryGrain, v: GranaryView) {
  const target = g.target >= 0 ? g.target : undefined;
  if (g.mode === 'auto') {
    if (g.rule === 'famine') return `开仓平粜：底仓也卖，按别处价的 ${percent(v.famineDiscount)} 要 ${price(g.sell)}，每天至多吞吐的量`;
    if (g.rule === 'short') return `常平：下一季已知歉收，停止春粜，把粮留着；收成后价贱时仍按 ${price(g.ruleBuy)} 收`;
    if (g.rule === 'reserve') return `常平：仓里只剩底仓，平年不再卖；收成后价贱时按 ${price(g.ruleBuy)} 收`;
    return `常平仓办法：收成后价贱时按 ${price(g.ruleBuy)} 收，青黄不接价贵时按 ${price(g.ruleSell)} 卖`;
  }
  if (g.mode === 'hold') return '存粮：不收也不卖';
  if (g.mode === 'buy') {
    const to = target !== undefined ? `收到 ${tons(target)}` : '收满仓';
    if (g.buy > 0) return `收粮：${g.limit > 0 ? `出价 ${price(g.limit)}` : `随市价，今天出 ${price(g.buy)}`}，${to}为止`;
    const done = target !== undefined ? g.held >= target - .5 : v.held >= v.capacity - .5;
    return done ? `收粮：已${to}，存着不卖` : '收粮：周边农户没有存粮可收';
  }
  const to = target ? `卖到剩 ${tons(target)}` : '卖空';
  return g.sell > 0 ? `卖粮：${g.limit > 0 ? `要价 ${price(g.limit)}` : `随市价，今天要 ${price(g.sell)}`}，${to}为止` : `卖粮：已${to}`;
}

/** A harvest's season as the farmers read it: unknown, normal, or a bad one and how much it leaves. */
const season = (h: GranaryGrain['outlook'][number]) => !h.known ? '年景未卜' : h.kind ? `${h.kind}，约 ${Math.round(h.yield * 10)} 成${h.event ? `（${h.event}）` : ''}` : '常年';

/** 年景：the next one or two harvests of the grain here. */
function Outlook({ g, v }: { g: GranaryGrain; v: GranaryView }) {
  if (!g.outlook.length) return null;
  return <p className="granary-outlook"><Term name="年景">收成前约两个月就看得出年景。已知下一季不到 {Math.round(v.shortHarvest * 10)} 成，常平就停止春粜，把粮留着。</Term>
    {g.outlook.map((h, i) => <span key={i} className={h.known && h.yield < v.shortHarvest ? 'bad' : ''}>{h.date}{h.in ? '（正在收）' : `（还有 ${h.days} 天）`}：{season(h)}</span>)}</p>;
}

/** 歉收将至、开仓平粜：the store's grains in trouble, said once above them. */
function Alerts({ v }: { v: GranaryView }) {
  const lines = v.grains.flatMap(g => {
    if (g.rule === 'famine') return [<p key={g.crop} className="granary-alert">开仓平粜：{g.name}在别处要 {price(g.marketSell)}，过了全年最贵的 {v.famineAt} 倍（{price(g.famineFrom)}）；常平把底仓也拿出来，按别处价的 {percent(v.famineDiscount)} 卖，每天至多吞吐的量。</p>];
    const bad = g.outlook.find(h => h.known && h.yield < v.shortHarvest && h.days >= 0);
    if (!g.short || !bad) return [];
    return [<p key={g.crop} className="granary-alert">歉收将至：{g.name}{bad.in ? '正在收' : `还有 ${bad.days} 天收`}，{season(bad)}。{g.mode === 'auto' ? '常平已停止春粜，把粮留着；粮价涨过全年最贵的 ' + v.famineAt + ' 倍时开仓平粜。' : '这种粮现在按你定的经营，不跟常平。'}</p>];
  });
  return lines.length ? <>{lines}</> : null;
}

/** 仓门：the porters' shift (吞吐) and 常平's reserve, for all the company's granaries in the 集散区. */
function Gate({ f, v }: { f: FacilityDetail; v: GranaryView }) {
  const [reserve, setReserve] = useState(String(Math.round(v.reserve * 100)));
  const r = parse(reserve);
  const ok = r !== undefined && !Number.isNaN(r) && r <= 100;
  const setGate = (args: Record<string, string>) => command('granary.gate', f.companyId, { market: f.marketId, ...args });
  const current = v.shifts.find(s => s.id === v.shift);
  return <div className="granary-gate">
    <div className="granary-gate-row">
      <b><Term name="吞吐">每天收进加发出合计有上限：规模 × 每级每天的吞吐 × 班次。白天米行、磨坊从仓里买走的先算，收市时农户送粮只能用剩下的。半班省下一半脚夫，双班多收发八成，脚夫加倍还要加点工钱。这家公司在本集散区的几座粮仓一起算。</Term></b>
      {f.canOperate ? <span className="seg small" role="group" aria-label="班次">{v.shifts.map(s => <button key={s.id} className={v.shift === s.id ? 'active' : ''} aria-pressed={v.shift === s.id}
        onClick={() => v.shift !== s.id && setGate({ shift: s.id })}>{s.name}</button>)}</span> : <span>{current?.name}</span>}
      <em>每天收发 {tons(v.throughput)} · 脚夫 {count(v.porters)} 人，工钱 {price(v.wagesPerWeek)}／周 · 昨天进出 {tons(v.lastDay)}</em>
    </div>
    <div className="granary-gate-row">
      <b><Term name="底仓">常平仓存七粜三：平年只卖到仓里剩这么多为止，留着防荒年。收粮、存粮、卖粮三种经营不看底仓。</Term></b>
      <span>容量的 {percent(v.reserve)}（{tons(v.capacity * v.reserve)}）{Math.abs(v.reserve - v.reserveDefault) < 1e-6 ? '，默认' : ''}</span>
      {f.canOperate && <span className="inline-form"><label>改成（%）<input value={reserve} onChange={e => setReserve(e.target.value.replace(/[^0-9.]/g, ''))} inputMode="decimal" style={{ width: 48 }} /></label>
        <button className="link" disabled={!ok} onClick={() => setGate({ reserve: String(r) })}>改</button></span>}
    </div>
  </div>;
}

function Grain({ f, g, v }: { f: FacilityDetail; g: GranaryGrain; v: GranaryView }) {
  const buying = g.buy > 0, selling = g.sell > 0;
  const works = g.crop === 'wheat' ? '磨坊' : '米行';
  return <div className="granary-grain">
    <div className="deposit-head"><b>{g.name}</b><em>{doing(g, v)}</em></div>
    <Outlook g={g} v={v} />
    {f.canOperate && <div className="seg small granary-ways" role="group" aria-label={`${g.name}怎么经营`}>
      {ways.map(w => <button key={w.id} className={g.mode === w.id ? 'active' : ''} aria-pressed={g.mode === w.id}
        onClick={() => g.mode !== w.id && setWay(f, g, w.id)}>{w.name}</button>)}
    </div>}
    {f.canOperate && (g.mode === 'buy' || g.mode === 'sell') && <WayForm key={`${g.mode}|${g.limit}|${g.target}`} f={f} g={g} />}
    <StatPairs rows={[
      ['仓里', tons(g.held)],
      ['收购价', buying ? `${price(g.buy)}／吨` : '不收'],
      ['卖价', selling ? `${price(g.sell)}／吨` : '不卖（存着）'],
      ['常年价', `${price(g.annual)}／吨（全年平均）`],
      ['收成后最贱', `约 ${price(g.low)}（${g.lowWhen}）`],
      ['收成前最贵', `约 ${price(g.high)}（${g.highWhen}）`],
      ['这个季节的常年价', `${price(g.season)}／吨`],
      ['每月损耗', percent(g.spoilage)],
    ]} />
    <p className="granary-now">现在：{g.marketBuy > 0 ? <>出价高过 <b>{price(g.marketBuy)}</b> 就有农户送粮（{g.marketBuyFrom}的地头价加车马费）</> : '周边农户没有存粮'}；
      要价不高过 <b>{price(g.marketSell)}</b>，{works}先买这里的粮（{g.marketSellFrom ? `别处最便宜的是${g.marketSellFrom}` : '别处买不到'}）。</p>
    <p className="deposit-terms">上周收进 {tons(g.boughtLastWeek)}（付 {pounds(g.paidLastWeek)}）{g.fromPlots.length ? `：${g.fromPlots.map(p => `${p.name} ${tons(p.tons)}`).join('、')}` : ''}；
      卖出 {tons(g.soldLastWeek)}（收 {pounds(g.revenueLastWeek)}）；损耗 {tons(g.spoiledLastWeek)}</p>
    {!!g.farmers.length && <ul className="offer-list compact granary-farmers">{g.farmers.map(x => {
      const brings = buying && x.net > x.ask;
      return <li key={x.plot} className={brings ? 'brings' : ''}>
        <span><b>{x.name}</b><em>存粮 {tons(x.stock)}，常年此时 {tons(x.normal)}</em></span>
        <span className="stock">地头价 {price(x.ask)}<em>车马 {price(x.haul)} · 到手 {buying ? price(x.net) : '—'}{brings ? ' · 会送粮' : ''}</em></span>
      </li>;
    })}</ul>}
  </div>;
}

export function GranaryPanel({ f }: { f: FacilityDetail }) {
  const g = f.granary;
  if (!g) return null;
  return <section className="panel-block">
    <div className="block-head"><h2><Term name="粮仓">每种粮选一种经营。常平：按常平仓的办法，在全年价格区间（收成后最贱到收成前最贵）的下四分之一处收、上四分之一处卖，收成后价贱时自己收进，青黄不接价贵时自己卖出；平年只卖到剩底仓为止，已知下一季歉收就不卖，荒年开仓平粜。收粮：随市价（比现在最便宜的到仓价高 {percent(g.step)}）或按自己的出价收，收满仓或收到定的数量为止，收的时候不卖。存粮：不收不卖。卖粮：随市价（与米行、磨坊在别处能买到的最便宜的粮同价，同价先买仓里的）或按自己的要价卖，卖空或卖到定的数量为止。收和卖都受每天的吞吐限制。周边农户看“收购价 − 车马费”，比自己的地头价高就送粮，送到地头价涨到这个到手价为止，一天至多送存粮的一成；几家粮仓同时收，农户送给到手价最高的一家。仓里的粮按月损耗，湿热的地方多，寒冷的地方少。</Term></h2>
      <span className="block-note">存 {tons(g.held)}／{tons(g.capacity)}</span></div>
    <Alerts v={g} />
    <Gate key={`${g.reserve}|${g.shift}`} f={f} v={g} />
    {g.grains.map(x => <Grain key={x.crop} f={f} g={x} v={g} />)}
  </section>;
}
