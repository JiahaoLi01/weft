import { useState } from 'react';
import { Hoverable } from '@/components/tooltip/TooltipSystem';
import { Term } from '@/components/Modal';
import { Select } from '@/components/Select';
import { command, count, percent, pounds, price } from '@/lib/format';
import { useUIStore } from '@/stores/ui-store';
import type { CompanyProduct, FinanceView, KnowledgeView, ProjectView, Quote, StageView, TechRow } from '@/types';

// 第四步的面板：报价、项目、设计改型、知识与研发、银行与契约。公司面板和设施面板共用。

/** 手艺换算（Production.md 第 1 节）：what the crews will know of the crafts new to them — 竹筛 90 → 转筒筛 71; a stage added: 手工擦米 28（新）. */
export function craftsText(q: Quote) {
  return (q.crafts ?? []).map(c => c.from ? `${c.from} ${Math.round(c.before)} → ${c.to} ${Math.round(c.after)}` : `${c.to} ${Math.round(c.after)}（新）`).join('；');
}

/** A project quote in one line: what it costs, what is due now, how long, what it scraps. */
export function QuoteText({ q }: { q: Quote }) {
  if (!q.ok) return <span className="quote-reason">{q.reason}</span>;
  const parts = [`约 ${pounds(q.total)}`];
  if (q.dueNow > 0) parts.push(`当付 ${pounds(q.dueNow)}`);
  parts.push(`约 ${Math.max(1, Math.round(q.days / 7))} 周`);
  if (q.scrap > 0) parts.push(`报废机床 ${percent(q.scrap)}`);
  if (q.crafts?.length) parts.push(`工人手艺 ${craftsText(q)}`);
  if (q.writeOff > 0) parts.push(`注销账面 ${pounds(q.writeOff)}`);
  return <span className="quote">{parts.join(' · ')}{q.orders && <em>设备：{q.orders}</em>}</span>;
}

const signedPercent = (v: number) => `${v > 0 ? '+' : v < 0 ? '−' : ''}${Math.round(Math.abs(v) * 100)}%`;

/** 付款节点：what each payment of a building pays for (a railway's rails and sleepers with the middle one, half bought at each
 *  end) and, once building, which are paid and the one due next (waiting: the project waits for its money). */
export function StageList({ stages, waiting, iron, timber, from, to }: { stages: StageView[]; waiting?: boolean; iron?: string; timber?: string; from?: string; to?: string }) {
  return <ol className="stage-list">{stages.map((s, i) => {
    const m = s.materials;
    const overrun = s.overrun && s.overrun > 0 ? `（含超支 ${pounds(s.overrun)}）` : s.overrun && s.overrun < 0 ? `（比报价省 ${pounds(-s.overrun)}）`
      : s.last && s.overrunMin !== undefined && s.overrunMax !== undefined ? `，另加超支 ${signedPercent(s.overrunMin)}～${signedPercent(s.overrunMax)}` : '';
    const when = s.status === 'paid' ? '已付' : s.status === 'next' ? (waiting ? '等钱' : s.dueDate ? `${s.dueDate} 付` : '下一笔') : s.dueDate ? `约 ${s.dueDate}` : '';
    return <li key={i} className={s.status ?? ''}>
      <b>{s.name}</b>
      <span>工程 {percent(s.share)} · {pounds(s.works)}{overrun}
        {m && <em>＋ 钢轨与枕木：{m.iron ?? iron}、{m.timber ?? timber}，{m.from ?? from}与{m.to ?? to}各买一半，本地卖家比模型外便宜的先买 · {m.bought ? `已买 ${pounds(m.cost)}` : `按今天的价最多 ${pounds(m.cost)}`}
          {` · 这一期合计${m.bought ? '' : '最多'} ${pounds(s.works + m.cost)}`}</em>}
      </span>
      {when && <i>{when}</i>}
    </li>;
  })}</ol>;
}

/** Why a project waits: what it cannot pay, how much is due against the company's cash, and for how long. */
function stallText(p: ProjectView) {
  const days = `已停 ${p.stalledDays} 天${p.stalledTotal && p.stalledTotal > p.stalledDays ? `（累计 ${p.stalledTotal} 天）` : ''}`;
  if (p.due === undefined || p.cash === undefined) return `付不出${p.stalledFor ?? '当期款项'}，${days}`;
  const short = p.due - p.cash;
  return `付不出${p.stalledFor ?? '当期款项'}：要 ${pounds(p.due)}，现金 ${pounds(p.cash)}${short > 0 ? `，还差 ${pounds(short)}` : ''} · ${days}`;
}

export function ProjectRow({ p, canOperate }: { p: ProjectView; canOperate: boolean }) {
  return <li className="project-row">
    <span><b>{p.name}</b><em>{p.kind} · {p.phase}{p.progress !== undefined ? ` ${percent(p.progress)}` : ''}{p.phaseEnd ? ` · 预计 ${p.phaseEnd}` : ''}</em></span>
    {p.progress !== undefined && <div className="phase-track"><span style={{ width: `${p.progress * 100}%` }} /></div>}
    <span className="project-money">已付 {pounds(p.paid)}{p.overrun > 0 ? ` · 超支 ${percent(p.overrun)}` : ''}{p.delay > 0 ? ` · 延误 ${p.delay} 天` : ''}</span>
    {!!p.stages?.length && <StageList stages={p.stages} waiting={p.stalled} />}
    {p.stalled && <span className="line-limit">{stallText(p)}</span>}
    {p.orders.map((o, i) => <span key={i} className="order-row">{o.units} {o.good} · {o.supplier} · {o.status}{o.status === '制造中' ? ` ${count(o.made)}/${o.units}` : ''}{o.arrive && o.status !== '已到' ? ` · ${o.arrive} 到` : ''}</span>)}
    {canOperate && <button className="link" onClick={() => command('project.cancel', p.id)}>撤销项目</button>}
  </li>;
}

export function ProductDesign({ p, canOperate }: { p: CompanyProduct; canOperate: boolean }) {
  const [open, setOpen] = useState(false);
  if (!p.design?.length) return null;
  return <span className="product-design">
    <span>设计：{p.design.map(d => `${d.component} ${d.tech}`).join(' · ')}{p.attributes ? `（${p.attributes}）` : ''}</span>
    {p.vehicle && <span className="block-note">{p.vehicle}</span>}
    {p.project && <ul className="project-list"><ProjectRow p={p.project} canOperate={canOperate} /></ul>}
    {canOperate && !p.project && !!p.redesigns?.length && <button className="link" onClick={() => setOpen(!open)}>{open ? '收起改型' : `可改型 ${p.redesigns.length} 项`}</button>}
    {open && <ul className="option-list">{(p.redesigns ?? []).map(r => <li key={r.id}><span><b>{r.component}：{r.name}</b><em>{r.effects}</em><QuoteText q={r.quote} /></span>
      {r.quote.ok && <button className="link" onClick={() => command('project.redesign', p.productId, { techs: r.id })}>改型</button>}</li>)}</ul>}
  </span>;
}

function LicenceForm({ company, tech }: { company: string; tech: TechRow }) {
  const [licensee, setLicensee] = useState(tech.licensees?.[0]?.id ?? '');
  const [fee, setFee] = useState(String(Math.round(tech.publicFee ?? 0)));
  const [royalty, setRoyalty] = useState('0');
  if (!tech.licensees?.length) return <span className="quote-reason">没有用得上它、又还没掌握它的公司。</span>;
  return <span className="licence-form">
    <Select value={licensee} onChange={setLicensee} ariaLabel="被授权方" options={tech.licensees.map(l => ({ value: l.id, label: l.name }))} />
    <label>许可费 £<input value={fee} onChange={e => setFee(e.target.value)} inputMode="decimal" /></label>
    <label>每单位分成 £<input value={royalty} onChange={e => setRoyalty(e.target.value)} inputMode="decimal" /></label>
    <button className="link" onClick={() => command('knowledge.offer_licence', company, { tech: tech.id, licensee, fee, royalty })}>开出授权</button>
  </span>;
}

function TechItem({ company, t, canOperate, researching }: { company: string; t: TechRow; canOperate: boolean; researching: boolean }) {
  const [licensing, setLicensing] = useState(false);
  const status = t.inUse ? '在用' : t.public ? '公共知识' : t.known ? `已掌握${t.source ? ` · ${t.source}` : ''}${t.disposal ? ` · ${t.disposal}` : ''}` : t.discovered ? '已发现' : '未发现';
  const progress = t.research + t.reverse + t.poach;
  return <li className={`tech-row ${t.known ? 'known' : t.discovered ? 'discovered' : 'unknown'}`}>
    <span><Hoverable content={<>{t.note}<br />技能要求：{t.skills}</>}><b className="term">{t.name}</b></Hoverable><em>{t.class} · {status}</em></span>
    <span className="tech-effects">{t.effects}</span>
    {t.patent && <span className="tech-patent">专利：{t.patent.mine ? '本公司' : t.patent.holder} · 至 {t.patent.expires}</span>}
    {!t.known && !t.discovered && t.pressure > 0 && <span className="tech-pressure">灵感压力 {percent(t.pressure)}／周</span>}
    {!t.known && progress > 0 && <span className="tech-progress">进度 {percent(progress)}（研发 {percent(t.research)} · 逆向 {percent(t.reverse)} · 挖人 {percent(t.poach)}）</span>}
    {!t.known && progress > 0 && <div className="phase-track"><span style={{ width: `${Math.min(1, progress) * 100}%` }} /></div>}
    {!t.known && t.quote && t.discovered && <span className="quote">研发报价 {pounds(t.quote.cost)} · {t.quote.weeks} 周{t.quote.nearest ? ` · 最近 ${t.quote.nearest} d=${t.quote.distance.toFixed(2)}` : ''}{t.quote.premium ? ' · 对手专利 ×1.3' : ''}</span>}
    {canOperate && <span className="tech-actions">
      {!t.known && t.quote?.ok && !researching && (t.class === '生产方式'
        ? <button className="link" onClick={() => command('knowledge.research', company, { tech: t.id })}>研发</button>
        : <><button className="link" onClick={() => command('knowledge.research', company, { tech: t.id, disposal: 'patent' })}>研发 · 申请专利</button>
            <button className="link" onClick={() => command('knowledge.research', company, { tech: t.id, disposal: 'hidden' })}>研发 · 保密</button></>)}
      {!t.known && t.canReverse && <Hoverable content={t.reverseReason}><button className="link" onClick={() => command('knowledge.reverse', company, { tech: t.id })}>逆向拆解</button></Hoverable>}
      {!t.known && t.canPoach && <Hoverable content={t.poachReason}><button className="link" onClick={() => command('knowledge.poach', company, { tech: t.id })}>挖人</button></Hoverable>}
      {t.canPatent && <button className="link" onClick={() => command('knowledge.dispose', company, { tech: t.id, choice: 'patent' })}>申请专利</button>}
      {t.canPublish && <button className="link" onClick={() => command('knowledge.dispose', company, { tech: t.id, choice: 'public' })}>公开</button>}
      {t.canPublish && <button className="link" onClick={() => setLicensing(!licensing)}>{licensing ? '收起授权' : '授权'}</button>}
    </span>}
    {licensing && <LicenceForm company={company} tech={t} />}
  </li>;
}

export function KnowledgePanel({ company, k, canOperate }: { company: string; k: KnowledgeView; canOperate: boolean }) {
  const openModal = useUIStore(s => s.openModal);
  const groups = new Map<string, TechRow[]>();
  for (const t of k.techs) groups.set(t.component, [...(groups.get(t.component) ?? []), t]);
  return <section className="archive-section knowledge"><h2><Term name="知识与研发">技术属于公司。先在生产中攒灵感发现，再出钱研发；逆向拆解最多 30%，挖人最多 24%。专利让对手研发多花 30%，14 年后成为公共知识；生产方式不能申请专利，只能隐藏、授权或公开。</Term></h2>
    <div className="place-actions"><button className="brush-button" onClick={() => openModal({ kind: 'tech-tree', company })}>科技树</button></div>
    {k.research ? <p className="research-status">在研：<b>{k.research.name}</b> · {percent(k.research.progress)} · 已付 {pounds(k.research.paid)}／{pounds(k.research.cost)}（{k.research.paidWeeks}/{k.research.weeks} 周）· 完成后{k.research.disposal}
      {k.research.stalled && <span className="line-limit"> · 现金不足，暂停付款</span>}
      {canOperate && <button className="link" onClick={() => command('knowledge.cancel_research', company)}>停止研发</button>}</p> : <p className="quiet">没有在研的技术。</p>}
    <div className="tech-columns">{[...groups.entries()].map(([component, techs]) => <div key={component} className="tech-group"><h3>{component}</h3>
      <ul className="tech-list">{techs.map(t => <TechItem key={t.id} company={company} t={t} canOperate={canOperate} researching={!!k.research} />)}</ul></div>)}</div>
    {!!k.licences.length && <><h3>授权</h3><ul className="tech-list">{k.licences.map(l => <li key={l.id} className="tech-row">
      <span><b>{l.tech}</b><em>{l.out ? `授予 ${l.other}` : `来自 ${l.other}`} · {l.status}{l.related ? ' · 关联方' : ''}{l.belowPublic ? ' · 低于公开价' : ''}</em></span>
      <span className="quote">许可费 {pounds(l.fee)} · 每单位分成 {price(l.royalty)} · 已收分成 {pounds(l.royaltiesPaid)}</span>
      {l.canAnswer && <span className="tech-actions"><button className="link" onClick={() => command('knowledge.accept_licence', l.id)}>接受</button><button className="link" onClick={() => command('knowledge.decline_licence', l.id)}>拒绝</button></span>}
    </li>)}</ul></>}
    {!!k.notices.length && <><h3>动态</h3><ul className="notice-list">{k.notices.map((n, i) => <li key={i}>{n.date} · {n.kind} {n.tech}{n.other ? `（${n.other}）` : ''}</li>)}</ul></>}
  </section>;
}

export function FinancePanel({ entity, f, canOperate }: { entity: string; f: FinanceView; canOperate: boolean }) {
  const borrowable = (f.banks ?? []).filter(b => b.available >= 1);
  return <section className="archive-section finance"><h2><Term name="银行与契约">银行只用自有现金放款。额度取“设施账面 50% + 存货 40%”与“经营利润能付得起的利息”中的小者；超出额度的部分加罚息、在 26 周内催收。付不出的利息并入本金；连续 12 周困境由债权人主导重组。</Term></h2>
    <dl>
      {f.credit && <><dt>经营利润／周</dt><dd>{pounds(f.credit.profitAvg)}<span className="detail-note">上周 {pounds(f.credit.lastOperating)}</span></dd></>}
      {f.fixedWeekly > 0 && <><dt>固定开支／周</dt><dd>{pounds(f.fixedWeekly)}</dd></>}
      <dt>贷款</dt><dd>{pounds(f.debt)}</dd>
      {f.credit && f.credit.distress > 0 && <><dt>困境</dt><dd className="warn">{f.credit.distress} / {f.credit.distressLimit} 周</dd></>}
      {f.credit && f.credit.restructured > 0 && <><dt>重组</dt><dd>{f.credit.restructured} 次</dd></>}
    </dl>
    {!!f.loans.length && <ul className="offer-list compact">{f.loans.map(l => <li key={l.id}>
      <span><b>{l.held ? `贷给 ${l.other}` : `欠 ${l.other}`}</b><em>年息 {percent(l.rate)} · {l.opened}{l.pledge ? ` · 抵押 ${l.pledge}` : ''}</em></span>
      <span className="price">{pounds(l.principal)}</span>
      {(l.overdueWeeks > 0 || l.callArrears > 0 || l.lastCalled > 0) && <span className="line-limit">{l.overdueWeeks > 0 ? `利息逾期 ${l.overdueWeeks} 周 ` : ''}{l.lastCalled > 0 ? `上周催收 ${pounds(l.lastCalled)} ` : ''}{l.callArrears > 0 ? `催收挂账 ${pounds(l.callArrears)}` : ''}</span>}
      {canOperate && !l.held && <span className="price-actions"><button className="link" onClick={() => command('finance.repay', entity, { loan: l.id, amount: String(Math.ceil(l.principal)) })}>还清</button>
        <button className="link" onClick={() => command('finance.repay', entity, { loan: l.id, amount: String(Math.ceil(l.principal / 4)) })}>还四分之一</button></span>}
    </li>)}</ul>}
    {canOperate && f.canBorrow && <>{borrowable.length ? <ul className="option-list">{borrowable.map(b => <li key={b.id}>
      <span><b>{b.name}</b><em>年息 {percent(b.rate)} · 额度 {pounds(b.limit)}（抵押 {pounds(b.collateral)}{b.coverageKnown ? ` · 覆盖 ${pounds(b.coverage)}` : ''}）· 可借 {pounds(b.available)}</em></span>
      <span className="price-actions">{[.25, .5, 1].map(s => <button key={s} className="link" onClick={() => command('finance.borrow', entity, { bank: b.id, amount: String(Math.floor(b.available * s)) })}>借 {pounds(b.available * s)}</button>)}</span>
    </li>)}</ul> : <p className="quiet">没有银行愿意再借：额度已满，或经营利润付不起利息。</p>}</>}
    {canOperate && !!f.pledgeable?.length && <ul className="option-list">{f.pledgeable.map(p => <li key={p.entity}>
      <span><b>抵押 {p.name}</b><em>{count(p.shares)} 股 · 净值 {pounds(p.value)} · 最多借 {pounds(p.max)}</em></span>
      {(f.banks ?? []).slice(0, 2).map(b => <button key={b.id} className="link" disabled={p.max < 1} onClick={() => command('finance.borrow', entity, { bank: b.id, amount: String(Math.floor(Math.min(p.max, b.lendable))), pledge: p.entity, pledgeVoting: String(p.voting), pledgeNonVoting: String(p.nonVoting) })}>向{b.name}借</button>)}
    </li>)}</ul>}
    {!!f.arrears.length && <ul className="notice-list warn">{f.arrears.map((a, i) => <li key={i}>{a.category} {pounds(a.amount)} · {a.creditor} · 自 {a.since}</li>)}</ul>}
    {f.book && <dl><dt>累计放款</dt><dd>{pounds(f.book.lent)}</dd><dt>收回本金</dt><dd>{pounds(f.book.repaid)}</dd><dt>利息收入</dt><dd>{pounds(f.book.interest)}</dd>
      <dt>并入本金的利息</dt><dd>{pounds(f.book.capitalized)}</dd><dt>核销</dt><dd>{pounds(f.book.writtenOff)}</dd><dt>可放款</dt><dd>{pounds(f.book.lendable)}</dd></dl>}
    {!!f.notices.length && <ul className="notice-list">{f.notices.map((n, i) => <li key={i}>{n.date} · {n.kind}{n.other ? ` ${n.other}` : ''}{n.amount ? ` ${pounds(n.amount)}` : ''}</li>)}</ul>}
  </section>;
}
