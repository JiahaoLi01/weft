import { DockContext, Modal, Cell } from '@/components/Modal';
import { useUIStore } from '@/stores/ui-store';
import { focusOn } from '@/lib/focus';
import { count, decimal, percent, pounds, price, shortPounds } from '@/lib/format';
import { CompanyModal } from '@/panels/CompanyModal';
import { FacilityModal } from '@/panels/FacilityModal';
import { ProductModal } from '@/panels/ProductModal';
import { GroupDashboard } from '@/panels/GroupDashboard';
import { ConstructModal } from '@/panels/ConstructModal';
import { PlotMarketModal } from '@/panels/PlotMarketModal';
import { TechTreeModal } from '@/panels/TechTreeModal';
import { GoodDossier } from '@/panels/GoodDossier';
import { FacilityMark, GoodArt, Seal } from './BusinessTree';
import type { TreeCompany, TreeFacility, TreeLine, TreeProduct, World } from '@/types';

// 档案（HierarchyUI.md 第 6 节）：the focus's full detail beside the business tree; the map stays live around it. Your own
// nodes open the operating panels; another group's open what your intel on it shows (第 3.1 节).

/** What each hidden field is, for the "unknown" list. */
const fieldNames: Record<string, string> = {
  cash: '现金', familyCash: '家族资金', lastOperating: '经营利润', profitAvg: '平均利润', book: '账面', debt: '贷款', history: '历史账目', unitCost: '单位成本',
  inputs: '投入品', bought: '进货', weekBought: '进货', lastWeekBought: '进货', shift: '班次', materials: '成本构成', conversion: '成本构成', piece: '成本构成',
  wages: '工资', upkeep: '维护', costParts: '成本构成', currentCost: '现价成本', workPerUnit: '加工费', margin: '毛利', marginPricing: '定价方式', marginTarget: '定价方式',
  crews: '班组', effective: '熟练度', research: '在研技术', knowledge: '知识', design: '设计', projects: '项目', finance: '契约', income: '收入构成', spending: '开支构成',
  inTransit: '在途', options: '经营选项', quotes: '报价', size: '规模', lines: '产线', markets: '在售地', share: '份额', units: '销量', revenue: '收入', status: '状态',
  ratio: '利用率', produced: '产出', nominal: '产能', lastWeekUnits: '销量', lastWeekRevenue: '收入',
};
const thresholds: Record<string, number> = { size: 20, lines: 20, markets: 20, share: 40, units: 40, revenue: 40, status: 40, ratio: 40, produced: 40, nominal: 40 };

function Unknown({ company }: { company: TreeCompany }) {
  const names = [...new Set(company.hiddenFields.map(f => fieldNames[f] ?? f))];
  if (!names.length) return null;
  const need = (f: string) => thresholds[f] ?? 60;
  const next = Math.min(...company.hiddenFields.map(need));
  return <section className="panel-block observed-unknown">
    <div className="block-head"><h2>未知</h2><span className="block-note">情报 {company.intel}，再到 {next} 才看得到更多</span></div>
    <p className="quiet">{names.join('、')}</p>
  </section>;
}

function findInTree(world: World): { company?: TreeCompany; facility?: TreeFacility; line?: TreeLine; product?: TreeProduct; brand?: TreeCompany['brands'][number] } {
  const f = world.focus;
  for (const c of world.tree?.companies ?? []) {
    if (f?.kind === 'company' && c.id === f.id) return { company: c };
    for (const b of c.brands) {
      if (f?.kind === 'brand' && b.id === f.id) return { company: c, brand: b };
      for (const p of b.products) if (f?.kind === 'product' && p.productId === f.id) return { company: c, brand: b, product: p };
    }
    for (const p of c.products) if (f?.kind === 'product' && p.productId === f.id) return { company: c, product: p };
    for (const fac of c.facilities) {
      if (f?.kind === 'facility' && fac.id === f.id) return { company: c, facility: fac };
      for (const l of fac.lines) if (f?.kind === 'line' && l.key === f.id) return { company: c, facility: fac, line: l };
    }
  }
  return {};
}

/** Another group's node: what the player's intel shows of it, and what stays unknown. */
function ObservedDossier({ world, onClose }: { world: World; onClose: () => void }) {
  const f = world.focus!;
  const { company, facility, line, product, brand } = findInTree(world);
  const title = line ? `${facility?.name} · ${line.id} 号线` : facility?.name ?? product?.product ?? brand?.name ?? company?.name ?? world.tree?.name ?? '';
  const unknown = '未知';
  return <Modal compact eyebrow={<>观察 · {world.tree?.name} · 情报 {f.intel}</>} title={title} onClose={onClose}
    meta={company && <button className="link" onClick={() => focusOn('company', company.id)}>{company.name}</button>}>
    {f.kind === 'group' && <section className="panel-block"><div className="block-head"><h2>旗下公司</h2></div>
      <ul className="observed-list">{(world.tree?.companies ?? []).map(c => <li key={c.id}><button className="link" onClick={() => focusOn('company', c.id)}><Seal text={c.emblem} hue={c.hue} />{c.name}</button><em>{c.business}</em></li>)}</ul></section>}
    {company && !facility && !product && !brand && <>
      <div className="cells"><Cell label="业务" value={company.business} /><Cell label="建筑" value={`${company.facilities.length} 处`} />
        <Cell label="产品" value={`${company.brands.reduce((n, b) => n + b.products.length, 0) + company.products.length} 种`} />
        <Cell label="现金" value={company.cash === undefined ? unknown : shortPounds(company.cash)} /></div>
      <section className="panel-block"><div className="block-head"><h2>建筑</h2></div>
        <ul className="observed-list">{company.facilities.map(x => <li key={x.id}><button className="link" onClick={() => focusOn('facility', x.id)}><FacilityMark typeId={x.typeId} type={x.type} />{x.name}</button><em>{x.type}{x.status ? ` · ${x.status}` : ''}</em></li>)}</ul></section>
    </>}
    {facility && <div className="cells"><Cell label="类型" value={facility.type} /><Cell label="状态" value={facility.status ?? unknown} />
      <Cell label="上周利用" value={facility.ratio === undefined ? unknown : facility.ratio < 0 ? '—' : percent(facility.ratio)} /><Cell label="产线" value={facility.lines.length ? `${facility.lines.length} 条` : '—'} /></div>}
    {line && <div className="cells"><Cell label="商品" value={<><GoodArt id={line.goodId} />{line.good}</>} />
      <Cell label="昨日产出" value={line.produced === undefined ? unknown : `${decimal(line.produced)} ${line.unit}`} note={line.nominal === undefined ? undefined : `额定 ${decimal(line.nominal)}`} /></div>}
    {product && <div className="cells"><Cell label="商品" value={<><GoodArt id={product.goodId} />{product.good}</>} /><Cell label="标价" value={`${price(product.price)}／${product.unit}`} />
      <Cell label="上周销量" value={product.units === undefined ? unknown : count(product.units)} /><Cell label="份额" value={product.share === undefined ? unknown : percent(product.share)} />
      <Cell label="上周收入" value={product.revenue === undefined ? unknown : pounds(product.revenue)} /></div>}
    {brand && !product && <section className="panel-block"><div className="block-head"><h2>产品</h2></div>
      <ul className="observed-list">{brand.products.map(p => <li key={p.productId}><button className="link" onClick={() => focusOn('product', p.productId)}><GoodArt id={p.goodId} />{p.product}</button><em>{price(p.price)}</em></li>)}</ul></section>}
    {company && <Unknown company={company} />}
  </Modal>;
}

/** The group's own dossier: the old group page (走势、控制路径、设立公司、待售的抵押股). */
function GroupDossier({ world, onClose }: { world: World; onClose: () => void }) {
  return <Modal eyebrow="集团" title={world.group?.name ?? world.tree?.name ?? '集团'} onClose={onClose} className="group-dossier">
    {world.group ? <GroupDashboard g={world.group} /> : <p className="quiet">正在读取集团的账目…</p>}
  </Modal>;
}

export function DossierHost({ world }: { world: World }) {
  const dossier = useUIStore(s => s.dossier);
  const close = useUIStore(s => s.closeDossier);
  const f = world.focus;
  if (!dossier || !f) return null;
  let body: React.ReactNode = null;
  if (f.kind === 'good') body = <GoodDossier world={world} onClose={close} />;
  else if (f.intel < 100) body = <ObservedDossier world={world} onClose={close} />;
  else if (f.kind === 'group') body = <GroupDossier world={world} onClose={close} />;
  else if (f.kind === 'company') body = <CompanyModal key={f.id} world={world} id={f.id} tab={dossier.tab} onClose={close} />;
  else if (f.kind === 'facility' || f.kind === 'line') {
    const [facility, number] = f.kind === 'line' ? f.id.split('#') : [f.id, undefined];
    const line = number !== undefined ? Number(number) : dossier.line;
    body = <FacilityModal key={`${facility}#${line ?? ''}`} world={world} id={facility} line={line} tab={dossier.tab} onClose={close} />;
  }
  else if (f.kind === 'product' || f.kind === 'brand')
    body = <ProductModal key={`${f.kind}:${f.id}:${dossier.market ?? ''}`} world={world} subject={f.kind} id={f.id} market={dossier.market} tab={dossier.tab} onClose={close} />;
  return <DockContext.Provider value={{ docked: true }}>{body}</DockContext.Provider>;
}

/** 工作台：a construction or a plot's competition, wide in the dossier's place. */
export function WorkbenchHost({ world }: { world: World }) {
  const modal = useUIStore(s => s.modal);
  const closeModal = useUIStore(s => s.closeModal);
  if (!modal) return null;
  // 科技树 fills the screen (a dialog over the map, which stops taking the wheel and the drag while it is open).
  if (modal.kind === 'tech-tree') return <TechTreeModal key={modal.company} world={world} company={modal.company} industry={modal.industry} onClose={closeModal} />;
  return <DockContext.Provider value={{ docked: true, wide: true }}>
    {modal.kind === 'plot-market' ? <PlotMarketModal key={modal.plot} world={world} plot={modal.plot} need={modal.need} onClose={closeModal} />
      : modal.kind === 'construct' ? <ConstructModal world={world} plot={modal.plot} company={modal.company} type={modal.type} onClose={closeModal} /> : null}
  </DockContext.Provider>;
}
