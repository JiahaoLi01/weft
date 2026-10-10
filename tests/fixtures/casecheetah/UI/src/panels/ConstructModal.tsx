import { Fragment, useEffect, useMemo, useRef, useState } from 'react';
import { Modal, Term } from '@/components/Modal';
import { Select } from '@/components/Select';
import { Figure, StatPairs } from '@/components/charts';
import { command, count, facilityGlyph, pounds, price, shortPounds } from '@/lib/format';
import { requestQuote, useUIStore, type QuoteReply } from '@/stores/ui-store';
import { QuoteBar } from './FacilityModal';
import { DepositCard } from './Mining';
import type { World } from '@/types';

/** Latest reply for a quote request; asks again when the request or the day changes, keeps the last reply meanwhile. */
export function useQuote(request: Record<string, string> | null, refresh: unknown): QuoteReply | undefined {
  const [id, setId] = useState('');
  const key = request ? JSON.stringify(request) : '';
  useEffect(() => {
    if (!request) return;
    const timer = setTimeout(() => setId(requestQuote(request)), 90);
    return () => clearTimeout(timer);
    // The request is captured through its key.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [key, refresh]);
  const reply = useUIStore(s => (id ? s.quotes[id] : undefined));
  const last = useRef<QuoteReply | undefined>(undefined);
  if (reply) last.current = reply;
  return reply ?? last.current;
}

export function ConstructModal({ world, plot, company, type, onClose }: { world: World; plot: string; company?: string; type?: string; onClose: () => void }) {
  const p = world.plot?.id === plot ? world.plot : undefined;
  const options = p?.build ?? [];
  // The company asked for, and its facility type when the 建筑 view chose one.
  const preferred = options.findIndex(o => (!company || o.companyId === company) && (!type || o.type === type));
  const firstIndex = Math.max(0, preferred >= 0 ? preferred : options.findIndex(o => !company || o.companyId === company));
  const [choice, setChoice] = useState(options[firstIndex] ? `${options[firstIndex].companyId}|${options[firstIndex].type}` : '');
  const option = options.find(o => `${o.companyId}|${o.type}` === choice) ?? options[firstIndex];
  const [size, setSize] = useState(1);
  const [method, setMethod] = useState('');
  const [supplier, setSupplier] = useState('');
  const [name, setName] = useState('');
  const [product, setProduct] = useState('');
  useEffect(() => { setMethod(''); setSupplier(''); setProduct(''); }, [choice]);
  const request = useMemo(() => option ? { kind: 'construct', company: option.companyId, type: option.type, plot, size: String(size), method, supplier, product } : null, [option, plot, size, method, supplier, product]);
  // A lease signed here (a block turning pending) asks for the quote again.
  const leases = (p?.resourcePoints ?? []).map(d => (d.blocks ?? []).map(b => b.status).join()).join('|');
  const reply = useQuote(request, `${world.time?.day}|${leases}`);
  const q = reply?.quote;
  const weeks = q ? Math.max(1, Math.round(q.days / 7)) : 0;
  const plotName = world.selection?.name ?? plot;
  const companies = Array.from(new Set(options.map(o => o.companyId)));
  // A mine's deposits on this plot, to lease a block right here.
  const deposits = option?.resource ? (p?.resourcePoints ?? []).filter(d => d.kindId === option.resource && d.leased) : [];
  return <Modal className="construct-modal" onClose={onClose}
    eyebrow={<>新建设施 · {plotName}{p?.market ? ` · ${p.market}` : ''}</>}
    title={option ? `${option.company} · ${option.typeName}` : '在此新建'}
    meta={<span><Term name="新建">只能建自己业务的设施；别的行业要在集团里另开姊妹公司。按节点付款：开工 30%、过半 40%、完工 30%，开工时掷出超支（−5%～+25%）与延误（0～6 周）。机器向机器厂按单订购：先付三成订金，运费由供应商承担，货到付清。</Term></span>}>
    {!options.length ? <div className="empty-call"><p>你的集团在这里没有能建设施的经营公司。</p><p className="quiet">在“集团与控制”里设立一家新公司（选好业务类型），再回到这个地块。</p></div> :
    <div className="modal-grid construct">
      <div className="modal-column">
        <section className="panel-block">
          <div className="block-head"><h2>谁来建、建什么</h2></div>
          {companies.map(id => <div key={id} className="build-group"><h3>{options.find(o => o.companyId === id)?.company}</h3>
            <ul className="choice-list">{options.filter(o => o.companyId === id).map(o => {
              const k = `${o.companyId}|${o.type}`;
              return <li key={k}><button className={k === `${option?.companyId}|${option?.type}` ? 'active' : ''} onClick={() => setChoice(k)}>
                <i className="glyph">{facilityGlyph[o.type] ?? o.typeName.slice(0, 1)}</i><b>{o.typeName}</b><span>{o.quote.ok ? `规模 1 约 ${shortPounds(o.quote.total)}` : o.quote.reason}</span></button></li>;
            })}</ul></div>)}
        </section>
        {!!deposits.length && <section className="panel-block deposits">
          <div className="block-head"><h2><Term name="矿藏与矿权">开矿要先租下这处矿藏的一块矿区；谈判中的租约也算，签了就能开工，矿建好时租约多半已经生效。</Term></h2></div>
          {deposits.map(d => <DepositCard key={d.id} d={d} />)}
        </section>}
      </div>
      <div className="modal-column">
        <section className="panel-block">
          <div className="block-head"><h2>方案</h2></div>
          <div className="form-grid">
            <label>名称（留空：{option?.typeName ?? ''}·{plotName}）<input value={name} onChange={e => setName(e.target.value)} /></label>
            <div className="field"><span>规模</span><div className="seg">{[1, 2, 3, 4, 5].map(s => <button key={s} className={size === s ? 'active' : ''} onClick={() => setSize(s)}>{s}</button>)}</div></div>
            {/* A store (粮仓) has a default in each of its components: it may start with one other tech, the rest default. */}
            {!!reply?.methods?.length && <div className="field"><span>生产方式</span><Select value={method} onChange={setMethod} ariaLabel="生产方式" options={[
              { value: '', label: reply.methods.filter(m => m.default).length > 1 ? '默认（各部件都用默认技术）' : `默认（${reply.methods.find(m => m.default)?.name ?? '基准'}）` },
              ...reply.methods.filter(m => !m.default).map(m => ({ value: m.id, label: `${m.name}${m.known ? '' : '（未掌握）'}`, disabled: !m.known }))]} /></div>}
            {(reply?.products?.length ?? 0) > 1 && <div className="field"><span>产品</span><Select value={product} onChange={setProduct} ariaLabel="产品" options={[
              { value: '', label: `默认（${reply!.products!.find(x => x.default)?.name ?? '最早的产品'}）` },
              ...reply!.products!.filter(x => !x.default).map(x => ({ value: x.id, label: `${x.name}${x.ready === false ? `（${x.readyDate} 可投产）` : ''}` }))]} /></div>}
            <div className="field"><span>机器供应商</span><Select value={supplier} onChange={setSupplier} ariaLabel="机器供应商" options={[
              { value: '', label: '自动：到岸价最低' }, ...(reply?.suppliers ?? []).map(s => ({ value: s.id, label: s.name })), { value: 'outside', label: '模型外（8 周，原价）' }]} /></div>
          </div>
          {method && reply?.methods && <p className="quiet">{reply.methods.find(m => m.id === method)?.effects}</p>}
        </section>
        {q && <section className="panel-block quote-block">
          <div className="block-head"><h2>报价</h2>{!q.ok && <span className="quote-reason">{q.reason}</span>}</div>
          {q.ok && <>
            <div className="quote-figures"><Figure value={shortPounds(q.total)} caption="总价（不含超支）" /><Figure value={`${weeks} 周`} caption="施工与交货" tone="second" /><Figure value={shortPounds(q.dueNow)} caption="开工当付" tone="second" /></div>
            <QuoteBar q={q} />
            <StatPairs rows={[['建筑', pounds(q.building)], ['机器（到岸）', pounds(q.machines)], ['付款节点', `开工 ${pounds(q.building * .3)} · 过半 ${pounds(q.building * .4)} · 完工 ${pounds(q.building * .3)}`]]} />
            {!!q.orderRows?.length && <div className="table machines"><span className="th">设备</span><span className="th">供应商</span><span className="th">到岸单价</span><span className="th">交货</span>
              {q.orderRows.map((o, i) => <Fragment key={i}><span>{count(o.units)} {o.unit}{o.good}</span><span>{o.supplier}</span><span>{price(o.unitPrice)}{o.freight > 0 ? <em>含运费 {price(o.freight)}</em> : null}</span><span>约 {o.leadDays} 天<em>{o.attributes}</em></span></Fragment>)}</div>}
          </>}
          <div className="confirm-row">
            <button className="action primary" disabled={!q.ok || !reply?.canOperate || !option} onClick={() => {
              if (!option) return;
              command('project.construct', option.companyId, { type: option.type, plot, size: String(size), method, supplier, name, product });
              onClose();
            }}>{q.ok ? `开工 · 先付 ${pounds(q.dueNow)}` : '不能开工'}</button>
            <span className="quiet">开工后在公司的“项目”里跟踪；建成的设施会出现在地图上。</span>
          </div>
        </section>}
      </div>
    </div>}
  </Modal>;
}
