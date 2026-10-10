import { Term } from '@/components/Modal';
import { price } from '@/lib/format';
import { tons } from './Mining';
import type { CropView } from '@/types';

// 农户存粮（Agriculture.md 第 2–3 节）：地块档案里，本地农户种什么、存粮比常年多少、地头价、哪天收成。

/** 七成, 十成. */
const tenths = (y: number) => `${'〇一二三四五六七八九十'[Math.max(0, Math.min(10, Math.round(y * 10)))]}成`;

/** One harvest's season: 未卜 until it is known, then 常年 or the bad season and what it leaves. */
function season(s: CropView['seasons'][number]) {
  const when = s.now ? '正在收' : `还有 ${s.days} 天`;
  if (!s.known) return `${s.date}（${when}）：年景未卜`;
  if (!s.kind) return `${s.date}（${when}）：常年`;
  return `${s.date}（${when}）：${s.kind}，约${tenths(s.yield ?? 1)}${s.event ? `（${s.event}）` : ''}`;
}

/** Dear or cheap against the season's normal price. */
const tone = (c: CropView) => c.ask >= c.expected * 1.25 ? 'dear' : c.ask <= c.expected * 0.85 ? 'cheap' : '';

function CropCard({ c }: { c: CropView }) {
  const next = c.nextHarvestDays === 0 ? '正在收' : `下一季 ${c.nextHarvestDays} 天后`;
  return <div className="deposit crop">
    <div className="deposit-head"><b>{c.name}</b><em>{c.zone}</em></div>
    <p className="deposit-facts">常年余粮 {tons(c.tonsPerYear)}／年 · 收成 {c.harvests.join('、')}（{next}）</p>
    {c.seasons.map((s, i) => <p key={i} className={`deposit-count ${s.known && s.kind ? 'dear' : ''}`}>{season(s)}</p>)}
    <p className="deposit-count">存粮 {tons(c.stock)}<span className="detail-note">常年此时 {tons(c.normalStock)}</span></p>
    <p className={`deposit-count ${tone(c)}`}>地头价 {price(c.ask)}／吨<span className="detail-note">常年 {price(c.normal)}，这个季节 {price(c.expected)}</span></p>
    {(c.soldLastWeek > 0 || c.eatenLastWeek > 0 || c.harvestLastWeek > 0) &&
      <p className="deposit-terms">上周收进 {tons(c.harvestLastWeek)}，卖出 {tons(c.soldLastWeek)}{c.householdsLastWeek > 0 ? `（其中家庭买去自碾 ${tons(c.householdsLastWeek)}）` : ''}{c.eatenLastWeek > 0 ? `，模型外吃掉 ${tons(c.eatenLastWeek)}` : ''}</p>}
  </div>;
}

export function Farmers({ crops }: { crops: CropView[] }) {
  if (!crops.length) return null;
  return <section className="archive-section deposits"><h2><Term name="农户">模型外的农民：收成季把粮食收进自家存粮，一年里陆续卖出。每季的年景在收成前两个月看得出来：旱、涝、风、寒、蝗按气候带出现，史实的灾年照史实，收成按年景打折。存粮比常年同期少，地头价就按金定律上涨（少一成约涨三成，少三成约涨一倍半），多了就跌；收成后最贱，青黄不接时最贵。米行、车马行按地头价加车马费向集散区里最便宜的农户买粮，买得多，存粮下去得快，价格也跟着上去。</Term></h2>
    {crops.map(c => <CropCard key={c.crop} c={c} />)}
  </section>;
}
