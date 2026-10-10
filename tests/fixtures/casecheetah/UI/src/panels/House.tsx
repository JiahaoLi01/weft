import { Term } from '@/components/Modal';
import { StatPairs } from '@/components/charts';
import { count, decimal, percent, price } from '@/lib/format';
import type { FacilityDetail } from '@/types';

// 商号（Trade.md 第 17 节）：账房写文书，文书决定这家公司在这个集散区能同时做几条进货线；每条线的货源、订货间隔、预计销量、在装的单子
// 与每天的利润。

const everyName = (days: number) => days === 30 ? '每月' : days === 91 ? '每季' : days === 182 ? '每半年' : `每 ${days} 天`;

export function HousePanel({ f }: { f: FacilityDetail }) {
  const h = f.house;
  if (!h) return null;
  return <>
    <section className="panel-block">
      <div className="block-head"><h2><Term name="商号">洋行、行栈：不存货（货存在集散仓），账房与伙计写文书。每单跨集散区的货用 {count(h.perOrder)} 份文书，付一次单据报关费（£{h.documents} × 起运国物价水平）；每批装船的货按货值付保险，海运 {percent(h.insuranceSea)}，内河与陆路 {percent(h.insuranceInland)}。文书最多存四周的产出：商号写多少，决定这家公司在这里能同时做几条进货线。</Term></h2>
        <span className="block-note">文书 {count(h.papers)}／{count(h.keep)} 份</span></div>
      <StatPairs rows={[
        ['每周写', `${count(h.weekly)} 份（每月约 ${decimal(h.monthly)} 份）`],
        ['进货线每月用', `${decimal(h.monthlyUse)} 份（${h.lanes.length} 条线）`, h.monthlyUse > h.monthly * .95 ? 'warn' : undefined],
        ['账房伙计', `${count(h.clerks)} 人，工钱 ${price(h.wagesPerWeek)}／周`],
        ['维护', `${price(h.upkeepPerWeek)}／周`],
      ]} />
    </section>
    <section className="panel-block">
      <div className="block-head"><h2><Term name="进货线">经销商每周看一次能做的生意：每种货到岸最便宜的几个货源，按货架价能卖多少，扣掉保险、单据与存货的损耗和资金成本以后每天赚多少；订货间隔在一月、一季、半年里取最划算的。按每份文书赚的钱排，文书够几条线就做几条。到了订货的日子，按预计销量补足一个间隔、路上的天数和一周余量的货，单子在间隔里从工厂仓库陆续装船。</Term></h2>
        <span className="block-note">{h.lanes.length} 条</span></div>
      {h.lanes.length ? <ul className="offer-list compact house-lanes">{h.lanes.map(l => <li key={l.product}>
        <span><b>{l.name}</b><em>{l.maker} · 从{l.from}{everyName(l.every)}订一次</em></span>
        <span className="stock">{decimal(l.forecast)} {l.unit}／日<em>{l.open > 0 ? `在装 ${count(l.open)} ${l.unit}` : `下次 ${l.next}`} · 每天赚 {price(l.profit)}</em></span>
      </li>)}</ul> : <p className="quiet">还没有进货线：要等这家公司每周看货的那天。</p>}
    </section>
  </>;
}
