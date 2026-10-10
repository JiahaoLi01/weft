import { useEffect, useMemo, useState } from 'react';
import { Term } from '@/components/Modal';
import { Select } from '@/components/Select';
import { HBar, StatPairs } from '@/components/charts';
import { command, count, decimal, percent, pounds, price } from '@/lib/format';
import { useQuote } from './ConstructModal';
import { ProjectRow, QuoteText, StageList } from './Step4';
import type { RailAccessRow, RailConcession, RailOpenLine, RailPlan, RailSegment, RailView } from '@/types';

// 线路（Transport.md 第 21 节）：铁路公司的铁路段、特许与修建、通行权，以及在相邻集散区之间选线报价。

const terrainColor = (build: number) => build >= 3 ? 'hard' : build >= 1.6 ? 'rough' : '';

/** The plots a line runs through, with each one's terrain and cost multiplier. */
function PlotPath({ plots }: { plots: { name: string; terrain: string; build: number }[] }) {
  if (!plots.length) return <span className="quiet">开局就有的线，没有逐地块的记录。</span>;
  return <span className="rail-path">{plots.map((p, i) => <span key={i} className={terrainColor(p.build)}>
    {i > 0 && <i>→</i>}{p.name}<em>{p.terrain} ×{p.build.toFixed(1)}</em></span>)}</span>;
}

/** An access agreement, from either side. */
function AccessRow({ a }: { a: RailAccessRow }) {
  return <li className="rail-access">
    <span><b>{a.holderSide ? a.user : a.lineName}</b><em>{a.holderSide ? `在本线开车` : `路权方 ${a.holder}`} · 过路费 {percent(a.toll)} 基准运价{a.tollPerTon !== undefined ? `（每吨 ${price(a.tollPerTon)}）` : ''}</em></span>
    <span className={`status-word ${a.status}`}>{a.statusName}{a.since ? ` · ${a.since} 起` : ''}</span>
    {(a.tollsPaid > 0 || a.carried > 0) && <span className="quiet">累计 {decimal(a.carried)} 吨 · 过路费 {pounds(a.tollsPaid)}</span>}
    <span className="price-actions">
      {a.canAnswer && <><button className="link" onClick={() => command('rail.accept_access', a.id)}>同意</button><button className="link" onClick={() => command('rail.decline_access', a.id)}>拒绝</button></>}
      {a.canEnd && <button className="link" onClick={() => command('rail.end_access', a.id)}>{a.status === 'requested' ? '撤回' : '终止'}</button>}
    </span>
  </li>;
}

function SegmentRow({ s, canOperate }: { s: RailSegment; canOperate: boolean }) {
  const [renaming, setRenaming] = useState(false);
  const [name, setName] = useState(s.name);
  return <li className="rail-segment">
    <div className="fleet-main">
      <span className="fleet-name"><b>{s.name}</b><em>{s.from} — {s.to} · {count(s.km)} 公里{s.opened ? ` · ${s.opened} 通车` : ''}</em></span>
      <span className="fleet-units">每吨基准 <b>{price(s.rate)}</b><em>账面 {pounds(s.book)} · 养护 {pounds(s.upkeepYear)}／年</em></span>
    </div>
    <PlotPath plots={s.plots} />
    <span className="quiet">自家 {count(s.trains)} 列在跑{s.otherTrains ? ` · 别家 ${count(s.otherTrains)} 列（通行权）` : ''}{s.carried !== undefined ? ` · 上周 ${decimal(s.carried)} 吨、运费 ${pounds(s.revenue ?? 0)}` : ''}</span>
    {!!s.users.length && <ul className="rail-sublist">{s.users.map(a => <AccessRow key={a.id} a={a} />)}</ul>}
    {canOperate && (renaming
      ? <span className="inline-form"><input value={name} onChange={e => setName(e.target.value)} style={{ width: 200 }} />
          <button className="link" disabled={!name.trim()} onClick={() => { command('rail.rename', s.id, { name: name.trim() }); setRenaming(false); }}>改名</button>
          <button className="link" onClick={() => setRenaming(false)}>取消</button></span>
      : <button className="link" onClick={() => setRenaming(true)}>改名</button>)}
  </li>;
}

function ConcessionRow({ k, canOperate }: { k: RailConcession; canOperate: boolean }) {
  return <li className="rail-concession">
    <div className="fleet-main">
      <span className="fleet-name"><b>{k.from} — {k.to}</b><em>{count(k.km)} 公里 · 修建难度 ×{k.difficulty.toFixed(2)} · 申请费 {pounds(k.fee)}</em></span>
      <span className={`status-word ${k.status}`}>{k.statusName}</span>
    </div>
    <span className="quiet">
      {k.status === 'pending' && `${k.applied} 申请，${k.decision} 批复。`}
      {k.status === 'granted' && `已批准，${k.expiry} 前要开工，约 ${k.buildWeeks} 周。工程 ${pounds(k.buildCost)} 分期付，钢轨与枕木过半时买：`}
      {k.status === 'built' && `已建成：${k.line ?? ''}。`}
      {k.status === 'lapsed' && '已作废（申请费不退）。'}
    </span>
    {k.status === 'granted' && !!k.stages?.length && <StageList stages={k.stages} iron={k.iron} timber={k.timber} from={k.from} to={k.to} />}
    {k.project && <ul className="project-list"><ProjectRow p={k.project} canOperate={canOperate} /></ul>}
    {canOperate && <span className="price-actions">
      {k.canBuild && <button className="action" onClick={() => command('rail.build', k.id)}>开工修建</button>}
      {k.canAbandon && <button className="link" onClick={() => command('rail.abandon', k.id)}>放弃特许</button>}
    </span>}
  </li>;
}

/** 选线：两个相邻集散区之间的路线、地形与报价；申请特许。 */
function Planner({ company, rail, day }: { company: string; rail: RailView; day?: number }) {
  const [from, setFrom] = useState(rail.markets[0]?.id ?? '');
  const neighbours = useMemo(() => rail.markets.find(m => m.id === from)?.neighbours ?? [], [rail.markets, from]);
  const [to, setTo] = useState(neighbours[0]?.id ?? '');
  useEffect(() => { if (!neighbours.some(n => n.id === to)) setTo(neighbours[0]?.id ?? ''); }, [neighbours, to]);
  const reply = useQuote(from && to ? { kind: 'rail', company, from, to } : null, day);
  const q = reply?.quote;
  const plan: RailPlan | undefined = reply?.rail && reply.rail.fromId === from && reply.rail.toId === to ? reply.rail : undefined;
  const underway = rail.concessions.some(k => ['pending', 'granted', 'building'].includes(k.status) &&
    ((k.fromId === from && k.toId === to) || (k.fromId === to && k.toId === from)));
  if (!rail.markets.length) return <p className="quiet">公司所在的国家没有相邻的集散区可修。</p>;
  return <div className="rail-planner">
    <div className="form-grid">
      <label className="field"><span>起点集散区</span><Select value={from} onChange={setFrom} ariaLabel="起点集散区" options={rail.markets.map(m => ({ value: m.id, label: m.name }))} /></label>
      <label className="field"><span>相邻的集散区</span><Select value={to} onChange={setTo} ariaLabel="终点集散区" options={neighbours.map(n => ({ value: n.id, label: n.name }))} /></label>
    </div>
    {q && !q.ok && <QuoteText q={q} />}
    {plan && q?.ok && <>
      <StatPairs rows={[['里程', `${count(plan.km)} 公里`], ['修建难度', `×${plan.difficulty.toFixed(2)}（按里程加权）`, plan.difficulty >= 2 ? 'negative' : ''],
        ['特许审批', `约 ${plan.approvalWeeks} 周`], ['修建', `约 ${plan.buildWeeks} 周`]]} />
      <div className="rail-route">{plan.plots.map((p, i) => <span key={p.id} className={terrainColor(p.build)}>
        {i > 0 && <i>→</i>}<b>{p.name}</b><em>{p.terrain} ×{p.build.toFixed(1)} · {count(p.km)} 公里</em></span>)}</div>
      <HBar parts={[
        { key: 'fee', label: '特许', value: plan.fee, color: '#b9a37a', detail: pounds(plan.fee) },
        { key: 'land', label: '征地', value: plan.land, color: '#8fb47c', detail: pounds(plan.land) },
        { key: 'earth', label: '土方桥隧', value: plan.earthworks, color: '#a8875a', detail: pounds(plan.earthworks) },
        { key: 'track', label: '铺轨', value: plan.track, color: '#6f9a92', detail: pounds(plan.track) },
        { key: 'stations', label: '车站', value: plan.stations, color: '#8a7fa8', detail: pounds(plan.stations) },
        { key: 'materials', label: '钢轨与枕木', value: plan.materials, color: '#9b6f62', detail: pounds(plan.materials) },
      ]} />
      <StatPairs rows={[['合计', pounds(plan.total)], ['申请时付', `特许费 ${pounds(plan.fee)}`], ['开工后分期付', `工程 ${pounds(plan.buildCost)}，钢轨与枕木约 ${pounds(plan.materials)}`]]} />
      <StageList stages={plan.stages} iron={plan.iron} timber={plan.timber} from={plan.from} to={plan.to} />
      {!!plan.existing.length && <p className="quote-reason">两地之间已有：{plan.existing.join('、')}。</p>}
      {underway ? <p className="quiet">这一段已经在申请或在修了：见左边的“特许与修建”。</p>
        : rail.canOperate && <button className="action" onClick={() => command('rail.apply', company, { from, to })}>申请特许（付 {pounds(plan.fee)}）</button>}
    </>}
  </div>;
}

/** 申请通行权：别家的铁路，从本公司有机务段的集散区出发。 */
function OpenLineRow({ l, company, rail }: { l: RailOpenLine; company: string; rail: RailView }) {
  const [toll, setToll] = useState(String(Math.round((l.ask > 0 ? l.ask : rail.askPermille / 1000) * 100)));
  const permille = Math.round(Number(toll) * 10);
  const valid = permille >= rail.minPermille && permille <= rail.maxPermille;
  return <li className="rail-open">
    <span><b>{l.name}</b><em>{l.from} — {l.to} · {count(l.km)} 公里 · 路权方 {l.holder} · 每吨基准 {price(l.rate)}</em></span>
    {l.state ? <span className="status-word">{l.state}</span> : rail.canOperate && <span className="inline-form">
      过路费 <input value={toll} onChange={e => setToll(e.target.value.replace(/[^0-9]/g, ''))} inputMode="numeric" style={{ width: 44 }} />% 基准运价
      {l.ask > 0 ? <em className="quiet">（对方要 {percent(l.ask)} 以上）</em> : <em className="quiet">（对方由玩家经营，等对方答复）</em>}
      <button className="link" disabled={!valid} onClick={() => command('rail.request_access', l.line, { company, toll: String(permille) })}>申请</button>
    </span>}
  </li>;
}

export function RailPanel({ company, rail, day }: { company: string; rail: RailView; day?: number }) {
  const live = rail.concessions.filter(k => k.status !== 'built' && k.status !== 'lapsed');
  const past = rail.concessions.filter(k => k.status === 'built' || k.status === 'lapsed');
  const [showPast, setShowPast] = useState(false);
  return <div className="modal-grid two rail-panel">
    <div className="modal-column">
      <section className="panel-block">
        <div className="block-head"><h2><Term name="铁路段">两个相邻集散区之间的一段铁路。持路权的公司独占它，别家的列车要有通行权、付过路费才能上线；线路的名字可以改，归属随公司转手。</Term></h2>
          <span className="block-note">{rail.segments.length} 段</span></div>
        {rail.segments.length ? <ul className="fleet-rows">{rail.segments.map(s => <SegmentRow key={s.id} s={s} canOperate={rail.canOperate} />)}</ul>
          : <p className="quiet">还没有自己的铁路段：右边选两个相邻的集散区申请特许、修一段；或向别家铁路申请通行权。</p>}
      </section>
      <section className="panel-block">
        <div className="block-head"><h2><Term name="特许与修建">申请特许先付申请费，所在国按它的审批周数批复（英国要议会私法案，约半年）。批准后在期限内开工：征地、土方桥隧、铺轨、车站按节点付款，钢轨与枕木在过半时向两端集散区买；工程有超支与延误。完工即通车，修建花费记为路轨账面，按年折旧。</Term></h2></div>
        {live.length ? <ul className="fleet-rows">{live.map(k => <ConcessionRow key={k.id} k={k} canOperate={rail.canOperate} />)}</ul> : <p className="quiet">没有在申请或在修的特许。</p>}
        {!!past.length && <button className="link rail-past" onClick={() => setShowPast(v => !v)}>{showPast ? '收起' : '显示'}已建成与已作废的特许（{past.length}）</button>}
        {showPast && <ul className="fleet-rows">{past.map(k => <ConcessionRow key={k.id} k={k} canOperate={rail.canOperate} />)}</ul>}
      </section>
    </div>
    <div className="modal-column">
      <section className="panel-block">
        <div className="block-head"><h2><Term name="修一段铁路">只连相邻的集散区，远的要一段段修过去。路线按里程 × 修建难度在两地的地块里找最省的走法；修建难度来自地形与土质（舆图上“修建难度”着色），有的地块根本修不了，例如青藏高原。</Term></h2></div>
        <Planner company={company} rail={rail} day={day} />
      </section>
      <section className="panel-block">
        <div className="block-head"><h2><Term name="通行权">在别家的铁路上开车：按基准运价的千分比付过路费，每运一吨付一次。电脑经营的路权方当场答复，达到它的要价就同意；玩家经营的路权方自己决定。任何一方都可以终止，终止后列车回机务段停泊。</Term></h2></div>
        {rail.access.length ? <ul className="fleet-rows">{rail.access.map(a => <AccessRow key={a.id} a={a} />)}</ul> : <p className="quiet">没有在别家铁路上的通行权。</p>}
        <h3>可以申请的线路（从本公司有机务段的集散区出发）</h3>
        {rail.open.length ? <ul className="fleet-rows">{rail.open.map(l => <OpenLineRow key={l.line} l={l} company={company} rail={rail} />)}</ul>
          : <p className="quiet">本公司有机务段的集散区里没有别家的铁路。</p>}
      </section>
    </div>
  </div>;
}
