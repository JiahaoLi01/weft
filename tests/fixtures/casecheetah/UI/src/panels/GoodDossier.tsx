import { Fragment } from 'react';
import { Cell, Modal, Term } from '@/components/Modal';
import { focusOn } from '@/lib/focus';
import { count, openCompany, percent, perUnit, price } from '@/lib/format';
import { FacilityMark, GoodArt } from '@/shell/BusinessTree';
import type { GoodDossierView, World } from '@/types';

// 商品档案（HierarchyUI.md 第 8.1 节）：a good's chain — what makes it from what, what it goes into — then who makes it and who
// uses it (as far as the player's intel shows), and the 集散区 where it is on sale.

/** A good in the chain: its picture and name; picking it moves the focus to it. */
function GoodChip({ id, name, amount }: { id: string; name: string; amount?: string }) {
  return <button className="good-chip" onClick={() => focusOn('good', id)}><GoodArt id={id} /><span>{name}</span>{amount && <em>{amount}</em>}</button>;
}

const roleNames: Record<string, string> = { use: '投入', equip: '装机', carry: '登记车船', store: '存粮' };

function Chain({ d }: { d: GoodDossierView }) {
  const self = <span className="chain-self"><GoodArt id={d.id} /><b>{d.name}</b></span>;
  return <section className="panel-block good-chain">
    <div className="block-head"><h2><Term name="供需链">上游：哪种设施用哪些投入做它（每单位用量），以及建这种设施要装的机器。下游：哪些设施拿它做什么、它是哪些设施要装的设备或登记的车船、满足家庭的哪些需求。点链上的商品看它的供需。</Term></h2></div>
    <div className="chain-side">
      <h3>上游 · 怎么来</h3>
      {d.outside && <p className="quiet">没有设施做它：来自模型外（农户、进口与本地小生产）。</p>}
      <ul className="chain-list">{d.made.map(m => <li key={m.typeId}>
        <span className="chain-type"><FacilityMark typeId={m.typeId} type={m.type} />{m.type}</span>
        <span className="chain-goods">{m.inputs.length ? m.inputs.map((i, k) => <span key={i.good}>{k > 0 && <i className="plus">+</i>}<GoodChip id={i.good} name={i.name} amount={`${perUnit(i.perUnit)} ${i.unit}`} /></span>) : <em className="quiet">不耗投入</em>}
          <i className="arrow">→</i>{self}</span>
        {!!m.equipment.length && <span className="chain-machines">装机：{m.equipment.map(e => <GoodChip key={e.good} id={e.good} name={e.name} amount={`×${e.count}`} />)}<em>每规模</em></span>}
      </li>)}</ul>
    </div>
    <div className="chain-side">
      <h3>下游 · 去哪里</h3>
      {!d.used.length && !d.equips.length && !d.carries.length && !d.stores.length && !d.needs.length && <p className="quiet">模型里没有设施或家庭需求用它。</p>}
      <ul className="chain-list">
        {d.used.map(u => <li key={`${u.typeId}:${u.good}`}><span className="chain-goods">{self}<em>{perUnit(u.perUnit)} {d.unit}</em><i className="arrow">→</i>
          <span className="chain-type"><FacilityMark typeId={u.typeId} type={u.type} />{u.type}</span><i className="arrow">→</i><GoodChip id={u.good} name={u.name} amount={`每${u.unit}`} /></span></li>)}
        {d.equips.map(e => <li key={`equip:${e.typeId}`}><span className="chain-goods">{self}<em>×{e.count} 每规模</em><i className="arrow">→</i>
          <span className="chain-type"><FacilityMark typeId={e.typeId} type={e.type} />{e.type}</span><em className="quiet">装进新建、扩建与换产</em></span></li>)}
        {d.carries.map(e => <li key={`carry:${e.typeId}`}><span className="chain-goods">{self}<i className="arrow">→</i>
          <span className="chain-type"><FacilityMark typeId={e.typeId} type={e.type} />{e.type}</span><em className="quiet">登记的车船</em></span></li>)}
        {d.stores.map(e => <li key={`store:${e.typeId}`}><span className="chain-goods">{self}<i className="arrow">→</i>
          <span className="chain-type"><FacilityMark typeId={e.typeId} type={e.type} />{e.type}</span><em className="quiet">收购、存放、卖出</em></span></li>)}
        {d.needs.map(n => <li key={`need:${n.id}`}><span className="chain-goods">{self}<i className="arrow">→</i><span className="chain-need">家庭 · {n.name}</span>
          <em className="quiet">每份 {perUnit(n.perUnit)} {d.unit}</em></span></li>)}
      </ul>
    </div>
  </section>;
}

export function GoodDossier({ world, onClose }: { world: World; onClose: () => void }) {
  const d = world.goodDossier;
  const f = world.focus;
  if (!d || d.id !== f?.id) return <Modal compact eyebrow="商品" title="…" onClose={onClose}><p className="quiet">正在读取商品档案…</p></Modal>;
  const fig = d.figures;
  const unknown = '未知';
  return <Modal eyebrow={<>商品 · {d.category}{d.buyers ? ` · ${d.buyers}买` : ''}</>} title={<span className="good-title"><GoodArt id={d.id} />{d.name}</span>}
    meta={<span className="quiet">单位：{d.unit}</span>} onClose={onClose} className="good-dossier">
    <div className="cells">
      <Cell label="上周产量" value={fig.output === undefined ? '模型外' : `${count(fig.output)} ${d.unit}`} note={`${fig.producers} 家在做`} />
      <Cell label="上周卖出" value={`${count(fig.sold)} ${d.unit}`} note={`${fig.users} 家在用`} />
      <Cell label="货架均价" value={fig.average === undefined ? '—' : `${price(fig.average)}／${d.unit}`} />
      <Cell label="在售集散区" value={`${fig.markets} 个`} />
      <Cell label={<Term name="每月损耗">存着的货每月按这个比例损耗：工厂的仓库、粮仓、集散仓里和路上都一样。湿热的地方（热带、亚热带湿润气候）多一些，寒冷的地方少一些；粮仓的仓储技术还能再减。</Term>}
        value={!d.stockable ? '不能存' : d.spoilage > 0 ? percent(d.spoilage) : '不损耗'}
        note={d.stockable && d.spoilage > 0 ? `湿热处 ×${d.humid}，寒冷处 ×${d.cold}` : undefined} />
    </div>
    <Chain d={d} />
    <section className="panel-block">
      <div className="block-head"><h2><Term name="生产者">做它的公司，本集团在前，其余产量大的在前。别家的产量要情报度 40 才看得到（大致产量）。</Term></h2><span className="block-note">{fig.producers} 家</span></div>
      {d.producers.length ? <div className="table good-producers"><span className="th">公司</span><span className="th">集团</span><span className="th num">建筑</span><span className="th num">上周产量</span><span className="th num">份额</span>
        {d.producers.map(p => <Fragment key={p.companyId}>
          <span className={p.mine ? 'mine' : ''}><button className="link" onClick={() => openCompany(p.companyId)}>{p.company}</button></span><span>{p.group ?? '独立'}</span><span className="num">{p.facilities}</span>
          <span className="num">{p.output === undefined ? unknown : `${count(p.output)} ${d.unit}`}</span><span className="num">{p.share === undefined ? (p.output === undefined ? unknown : '—') : percent(p.share)}</span></Fragment>)}</div>
        : <p className="quiet">没有公司做它。</p>}
      {fig.producers > d.producers.length && <p className="quiet">另有 {fig.producers - d.producers.length} 家。</p>}
    </section>
    <section className="panel-block">
      <div className="block-head"><h2><Term name="用户">用它的公司：当投入、装进设施、登记成车船或收进粮仓。别家的进货量要情报度 60 才看得到。</Term></h2><span className="block-note">{fig.users} 家</span></div>
      {d.users.length ? <div className="table good-users"><span className="th">公司</span><span className="th">集团</span><span className="th">用作</span><span className="th num">建筑</span><span className="th num">上周进货</span>
        {d.users.map(u => <Fragment key={u.companyId}>
          <span className={u.mine ? 'mine' : ''}><button className="link" onClick={() => openCompany(u.companyId)}>{u.company}</button></span><span>{u.group ?? '独立'}</span><span>{roleNames[u.role] ?? u.role}</span>
          <span className="num">{u.facilities}</span><span className="num">{u.bought === undefined ? unknown : `${count(u.bought)} ${d.unit}`}</span></Fragment>)}</div>
        : <p className="quiet">没有公司用它{d.needs.length ? '；它卖给家庭' : ''}。</p>}
      {fig.users > d.users.length && <p className="quiet">另有 {fig.users - d.users.length} 家。</p>}
    </section>
    <section className="panel-block">
      <div className="block-head"><h2><Term name="各集散区">货架上的最低价与按存货加权的均价、存货、上周卖出、卖家数与主力商品（上周卖得最多的一款）。卖得多的在前。</Term></h2></div>
      {d.markets.length ? <div className="table good-markets"><span className="th">集散区</span><span className="th num">最低价</span><span className="th num">均价</span><span className="th num">存货</span>
        <span className="th num">上周卖出</span><span className="th num">卖家</span><span className="th">主力商品</span>
        {d.markets.map(m => <Fragment key={m.market}><span>{m.name}</span><span className="num">{m.lowest >= 0 ? price(m.lowest) : '—'}</span><span className="num">{m.average >= 0 ? price(m.average) : '—'}</span>
          <span className="num">{count(m.stock)}</span><span className="num">{count(m.units)}</span><span className="num">{m.sellers}</span><span>{m.main ?? '—'}</span></Fragment>)}</div>
        : <p className="quiet">哪里都没有在卖。</p>}
    </section>
  </Modal>;
}
