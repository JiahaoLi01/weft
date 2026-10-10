import { useEffect, useState } from 'react';
import { Term } from '@/components/Modal';
import { Select } from '@/components/Select';
import { command, count, pounds, price } from '@/lib/format';
import type { DepositBlock, DepositView, LeaseView, MiningView } from '@/types';

// 基础资源与采矿权（Resources.md 第 8 节）：地块档案的矿藏，公司面板的矿权。

/** 500 万吨, 2.4 万吨, 830 吨. */
export const tons = (value: number) => value >= 10000 ? `${(value / 10000).toFixed(value >= 1e6 ? 0 : 1)}\u00a0万吨` : `${count(value)}\u00a0吨`;

/** Who holds a block, until when. */
function holding(b: DepositBlock) {
  if (b.status === 'exhausted') return '已采完';
  if (!b.company) return `空着，剩 ${tons(b.left)}`;
  return `${b.company}${b.status === 'pending' ? `（${b.statusName}，${b.until ?? ''} 定）` : b.until ? `，到 ${b.until}` : '，永久'}`;
}

export function DepositCard({ d }: { d: DepositView }) {
  const lessees = d.lessees ?? [];
  const [company, setCompany] = useState(lessees[0]?.id ?? '');
  useEffect(() => { if (!lessees.some(l => l.id === company)) setCompany(lessees[0]?.id ?? ''); }, [lessees, company]);
  const lessee = lessees.find(l => l.id === company);
  const blocks = d.blocks ?? [];
  const held = blocks.filter(b => b.company);
  const free = blocks.filter(b => b.status === 'free').length, done = blocks.filter(b => b.status === 'exhausted').length;
  const facts = [...d.attributes.map(a => `${a.name} ${a.value}${a.percent ? '%' : ''}`), d.difficulty !== 1 ? `开采难度 ×${d.difficulty.toFixed(2)}` : '',
    d.depletes ? `剩余 ${tons(d.left)}` : ''].filter(Boolean);
  return <div className="deposit">
    <div className="deposit-head"><b>{d.name}</b><em>{d.kind}{d.variety ? ` · ${d.variety}` : ''}</em></div>
    {!!facts.length && <p className="deposit-facts">{facts.join(' · ')}</p>}
    {d.leased && <>
      <ol className="block-strip" aria-label={`${blocks.length} 块矿区`}>{blocks.map(b =>
        <li key={b.index} className={`${b.status}${b.mine ? ' mine' : ''}`} aria-label={`第 ${b.index + 1} 块：${holding(b)}`} />)}</ol>
      <p className="deposit-count">{blocks.length} 块，每块 {tons(d.blockTons ?? 0)}：空着 {free}{held.length ? `，租出 ${held.length}` : ''}{done ? `，采完 ${done}` : ''}</p>
      {!!held.length && <ul className="deposit-holders">{held.map(b => <li key={b.index} className={b.mine ? 'mine' : ''}>第 {b.index + 1} 块 · {holding(b)}</li>)}</ul>}
      {d.terms ? <p className="deposit-terms">
        <Term name={d.terms.regime}>{d.terms.merge
          ? '矿归地主：签约付律师与勘测费；每采一吨交矿租（签约时外购价的一成上下，租期内不变）；不开采也要交保底年租，当期的矿租可以抵扣。每 28 天结算，连欠三期地主收回。'
          : '矿归官府：申请时付申请与勘测费；每采一吨交比例矿税，另交按年的固定矿税。每 28 天结算，连欠三期官府收回。'}</Term>
        ：交{d.terms.lessor}签约费 {pounds(d.terms.fee)} · 每吨 {price(d.terms.royaltyPerTon)} · {d.terms.merge ? '保底年租' : '固定矿税'} {pounds(d.terms.rentPerYear)}／年 ·
        {d.terms.termYears ? ` ${d.terms.termYears} 年` : ' 永久'} · {d.terms.merge ? '谈判' : '审批'}约 {d.terms.weeks} 周</p>
        : d.termsReason && <p className="quiet">{d.termsReason}</p>}
      {!!lessees.length && d.terms && <div className="deposit-lease">
        {lessees.length > 1 && <Select value={company} onChange={setCompany} ariaLabel="租矿的公司" minWidth={170}
          options={lessees.map(l => ({ value: l.id, label: `${l.name}（现金 ${pounds(l.cash)}）` }))} />}
        <button className="action" disabled={!lessee?.ok} onClick={() => lessee && command('mine.lease', lessee.id, { point: d.id, block: String(lessee.block ?? '') })}>
          {lessee?.ok ? `${lessees.length > 1 ? '' : `${lessee.name} `}租下第 ${(lessee.block ?? 0) + 1} 块` : '租下一块'}</button>
        {lessee && !lessee.ok && <span className="quiet">{lessee.reason}</span>}
      </div>}
    </>}
  </div>;
}

/** 地块档案的“矿藏”：每处矿藏的品种、属性、难度与剩余，矿区由谁租着，按本国制度租一块。 */
export function Deposits({ deposits }: { deposits: DepositView[] }) {
  if (!deposits.length) return null;
  return <section className="archive-section deposits"><h2><Term name="矿藏">资源点是地块的属性。煤田与铁矿分块租出：英国、美国向地主租矿，别国向官府申请特许；开矿要先租下这处矿藏的一块矿区，按块开采。还没发现的矿藏不显示。</Term></h2>
    {deposits.map(d => <DepositCard key={d.id} d={d} />)}
  </section>;
}

function LeaseRow({ l, forfeitMissed }: { l: LeaseView; forfeitMissed: number }) {
  const ended = l.status !== 'pending' && l.status !== 'active';
  return <li className="lease-row">
    <div className="fleet-main">
      <span className="fleet-name"><b>{l.pointName} · 第 {l.block + 1} 块</b><em>{l.plotName} · {l.regime}，交{l.lessor}</em></span>
      <span className={`status-word ${l.status}`}>{l.statusName}</span>
    </div>
    <span className="quiet">
      {l.status === 'pending' && `${l.since} 签约，约 ${l.decision} ${l.lessor === '官府' ? '批复' : '谈妥'}；签约费 ${pounds(l.fee)} 已付。`}
      {l.status === 'active' && `${l.since} 起${l.until ? `，到 ${l.until}` : '，永久有效'}。`}
      {ended && `${l.until ?? ''} 结束。`}
    </span>
    <span>每吨 {price(l.royaltyPerTon)} · {l.merge ? '保底年租' : '固定矿税'} {pounds(l.rentPerYear)}／年 · 本块剩 {tons(l.left)} · 已采 {tons(l.raised)} · 累计交 {pounds(l.paid)}</span>
    {l.lastDue > 0 && <span className={l.lastPaid < l.lastDue ? 'warn' : 'quiet'}>上期应交 {pounds(l.lastDue)}，实交 {pounds(l.lastPaid)}
      {l.missed > 0 && !ended ? `；已连欠 ${l.missed} 期，欠满 ${forfeitMissed} 期收回` : ''}</span>}
    {(l.canRenew || l.canSurrender || l.renewFrom) && !ended && <span className="price-actions">
      {l.canRenew && <button className="link" onClick={() => command('mine.renew', l.id)}>续租（再付签约费，按今天的条件）</button>}
      {!l.canRenew && l.renewFrom && <span className="quiet">{l.renewFrom} 起可以续租</span>}
      {l.canSurrender && <button className="link" onClick={() => command('mine.surrender', l.id)}>放弃这块</button>}
    </span>}
  </li>;
}

/** 公司面板的“矿权”页签。 */
export function MiningPanel({ mining }: { mining: MiningView }) {
  const live = mining.leases.filter(l => l.status === 'pending' || l.status === 'active');
  const past = mining.leases.filter(l => l.status !== 'pending' && l.status !== 'active');
  return <div className="modal-grid one"><section className="panel-block">
    <div className="block-head"><h2><Term name="矿权">一份矿权是一处矿藏的一块矿区。矿按块开采，块号小的先采；每吨的矿租记进出矿的成本，每 {mining.settleDays} 天连同年租一起结算。最后两年可以续租；采完、期满、放弃或欠满 {mining.forfeitMissed} 期后矿区空出来。</Term></h2>
      {mining.weeklyRent > 0 && <span className="quiet">年租合计每周约 {pounds(mining.weeklyRent)}</span>}</div>
    {live.length ? <ul className="fleet-rows">{live.map(l => <LeaseRow key={l.id} l={l} forfeitMissed={mining.forfeitMissed} />)}</ul>
      : <p className="quiet">还没有矿权。在舆图上选一处有煤田或铁矿的地块，在地块档案的“矿藏”里租一块矿区。</p>}
    {!!past.length && <><h3 className="rail-past">已结束的矿权</h3><ul className="fleet-rows">{past.map(l => <LeaseRow key={l.id} l={l} forfeitMissed={mining.forfeitMissed} />)}</ul></>}
  </section></div>;
}
