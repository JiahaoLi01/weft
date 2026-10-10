import { Term } from '@/components/Modal';
import { sendToUE } from '@/lib/ue-bridge';
import { people, percent } from '@/lib/format';
import { FacilityMark } from './BusinessTree';
import type { CountryArchiveView } from '@/types';

// 国家档案（HierarchyUI.md 第 8.2 节）：a country's people and land, its 集散区, its cultures, the companies registered in it and
// the buildings on its land. Its name is running data: a renamed or reassigned country shows as it is now.

export function CountryArchive({ c }: { c: CountryArchiveView }) {
  return <div className="place-archive country-archive">
    <div className="panel-heading"><small>国家档案</small><h1>{c.name || '归属待校订'}</h1></div>
    <section className="archive-section"><h2>概览</h2><dl>
      {c.population !== undefined && <><dt><Term name="人口">各地块的人口之和（HYDE／ISIMIP 推回 1844 年）。</Term></dt><dd>{people(c.population)}</dd></>}
      <dt>地块</dt><dd>{c.plots} 块</dd>
      <dt>集散区</dt><dd>{c.markets.length} 个</dd>
      <dt>境内建筑</dt><dd>{c.buildings} 处{c.mine ? <span className="detail-note">本集团 {c.mine} 处</span> : null}</dd>
      <dt>注册的公司</dt><dd>{c.registered} 家</dd>
    </dl></section>
    {!!c.cultures.length && <section className="archive-section"><h2>主要文化</h2>
      <ul className="share-list">{c.cultures.map(x => <li key={x.name}><span>{x.name}<em>{x.group}</em></span><b>{percent(x.share)}</b></li>)}</ul></section>}
    <section className="archive-section"><h2><Term name="集散区">人多的在前。点一个飞过去，下面换成它的档案。</Term></h2>
      <ul className="country-markets">{c.markets.map(m => <li key={m.id}>
        <button className="link" onClick={() => m.lon !== undefined && m.lat !== undefined && sendToUE('focus_market', { id: m.id, name: m.name, lon: m.lon, lat: m.lat })}>{m.name}</button>
        <em>{m.plots} 块{m.population !== undefined ? ` · ${people(m.population)}` : ''}</em></li>)}</ul></section>
    {!!c.groups.length && <section className="archive-section"><h2>在这里注册的公司</h2>
      <ul className="share-list">{c.groups.map(g => <li key={g.groupId || 'independent'}><span className={g.mine ? 'mine' : ''}>{g.group}<em>{g.names}{g.count > 4 ? ' 等' : ''}</em></span><b>{g.count} 家</b></li>)}</ul></section>}
    {!!c.types.length && <section className="archive-section"><h2>境内的建筑</h2>
      <ul className="country-types">{c.types.map(t => <li key={t.typeId}><FacilityMark typeId={t.typeId} type={t.type} /><span>{t.type}</span><b>{t.count}</b></li>)}</ul></section>}
  </div>;
}
