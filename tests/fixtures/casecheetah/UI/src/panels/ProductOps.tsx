import { Fragment, useEffect, useMemo, useState } from 'react';
import { MoneyInput, Term } from '@/components/Modal';
import { Select } from '@/components/Select';
import { HBar } from '@/components/charts';
import { command, percent, pounds, price } from '@/lib/format';
import { useUIStore } from '@/stores/ui-store';
import { useQuote } from './ConstructModal';
import { StageEditor, chainOf, chainSummary, chosenOf } from './Stages';
import { ProjectRow, QuoteText, craftsText } from './Step4';
import type { AllocationRow, Company, CompanyProduct, DesignComponent, DesignQuote, FacilityDetail, Quote } from '@/types';

// 产品、品牌与产线分配（《公司治理设计》“品牌与产品”；《知识产权设计》“设计新型号只是把已掌握的部件技术组合起来”）。

const shareColors = ['#c9a96a', '#7fb3a8', '#9d8fc0', '#b98a6e', '#8796a0', '#a3b07a'];
const step = 5;

/** Even shares in whole percent; the first takes the rounding. */
function evenShares(ids: string[]): Record<string, number> {
  const out: Record<string, number> = {};
  ids.forEach((id, i) => { out[id] = Math.floor(100 / ids.length) + (i === 0 ? 100 % ids.length : 0); });
  return out;
}

/** 产线分配 of one good: a works' line (line = its number), or a service's capacity of the good. */
export function AllocationGood({ f, a, line }: { f: FacilityDetail; a: AllocationRow; line?: number }) {
  const openModal = useUIStore(s => s.openModal);
  const server = useMemo(() => Object.fromEntries(a.parts.map(p => [p.productId, Math.round(p.share * 100)])), [a.parts]);
  const serverKey = a.parts.map(p => `${p.productId}:${Math.round(p.share * 100)}`).join(',');
  const [shares, setShares] = useState<Record<string, number>>(server);
  useEffect(() => setShares(server), [serverKey]); // eslint-disable-line react-hooks/exhaustive-deps
  const ids = Object.keys(shares).sort();
  const total = ids.reduce((s, id) => s + shares[id], 0);
  const dirty = ids.map(id => `${id}:${shares[id]}`).join(',') !== [...a.parts].sort((x, y) => x.productId.localeCompare(y.productId)).map(p => `${p.productId}:${Math.round(p.share * 100)}`).join(',');
  const known = [...a.parts, ...(a.options ?? [])];
  const row = (id: string) => known.find(p => p.productId === id);
  const nudge = (id: string, d: number) => setShares(s => ({ ...s, [id]: Math.max(0, Math.min(100, s[id] + d)) }));
  const join = (id: string) => setShares(s => evenShares([...Object.keys(s), id].sort()));
  const leave = (id: string) => setShares(s => { const rest = Object.keys(s).filter(x => x !== id).sort(); return evenShares(rest); });
  const where: Record<string, string> = line !== undefined ? { line: String(line) } : { good: a.good };
  const apply = () => command('facility.allocate', f.id, { ...where, shares: ids.filter(id => shares[id] > 0).map(id => `${id}:${shares[id] * 10}`).join(',') });
  const same = (a.options ?? []).filter(o => o.sameDesign && !(o.productId in shares));
  const other = (a.options ?? []).filter(o => !o.sameDesign);
  return <div className="allocation-good">
    <div className="allocation-head"><b>{a.goodName}</b><span className="block-note">{ids.length > 1 ? `${ids.length} 个产品共用${line !== undefined ? '这条线' : '这份产能'}` : ids.length ? `整条${line !== undefined ? '线' : '产能'}做一个产品` : '还没有产品'}</span></div>
    <HBar parts={ids.map((id, i) => ({ key: id, label: row(id)?.product ?? id, value: Math.max(shares[id], 0.0001), color: shareColors[i % shareColors.length], detail: `${shares[id]}%` }))} />
    <ul className="allocation-parts">{ids.map(id => { const p = row(id); return <li key={id}>
      <span className="part-name"><button className="link" onClick={() => openModal({ kind: 'product', subject: 'product', id })}>{p?.product ?? id}</button><em>{p?.brand || '无品牌'}</em></span>
      {f.canOperate ? <span className="share-stepper">
        <button aria-label="减少" disabled={shares[id] <= 0} onClick={() => nudge(id, -step)}>−</button><b>{shares[id]}%</b><button aria-label="增加" disabled={shares[id] >= 100} onClick={() => nudge(id, step)}>+</button>
      </span> : <b>{shares[id]}%</b>}
      {f.canOperate && ids.length > 1 && <button className="link" onClick={() => leave(id)}>移出</button>}
    </li>; })}</ul>
    {f.canOperate && dirty && <div className="allocation-apply">
      {total === 100 ? <button className="action" onClick={apply}>按此分配</button> : <span className="quote-reason">合计 {total}%，须为 100%</span>}
      <button className="link" onClick={() => setShares(server)}>还原</button>
    </div>}
    {f.canOperate && !!same.length && <div className="allocation-options"><h3>设计相同，可以并线</h3>
      <ul className="option-list">{same.map(o => <li key={o.productId}><span><b>{o.product}</b><em>{o.brand || '无品牌'}{o.ready === false ? ` · 工程验证中，${o.readyDate} 可投产` : ''}</em></span>
        {o.ready !== false && <button className="link" onClick={() => join(o.productId)}>并线</button>}</li>)}</ul></div>}
    {f.canOperate && !!other.length && <div className="allocation-options"><h3><Term name="设计不同，要换产">换产按新旧设计的距离报废机床、付安装费、停产几周，工人对新设计的手艺从旧设计按技术距离折算；完工后整条线只做这个产品，原来共线的产品离开这条线。别的商品的产品也可以：这条线改做那种商品。别的线照常生产。</Term></h3>
      <ul className="option-list">{other.map(o => <li key={o.productId}><span><b>{o.product}</b><em>{o.goodName && o.goodName !== a.goodName ? `改做${o.goodName} · ` : ''}{o.brand || '无品牌'}</em>
        {o.ready === false ? <span className="quote-reason">工程验证中，{o.readyDate} 可投产</span> : o.quote && <QuoteText q={o.quote} />}
        {!!o.quote?.dropped?.length && <span className="quote-reason">离开这条线：{o.quote.dropped.join('、')}</span>}</span>
        {o.quote?.ok && <button className="link" onClick={() => command('project.retool', f.id, line !== undefined ? { line: String(line), product: o.productId } : { product: o.productId })}>换产到这个产品</button>}</li>)}</ul></div>}
  </div>;
}

/** 产线分配: what the works makes of each output, and the other products it could make. */
export function AllocationPanel({ f }: { f: FacilityDetail }) {
  if (!f.allocation?.length) return null;
  return <section className="panel-block allocation">
    <div className="block-head"><h2><Term name="产能分配">这处设施的每种服务卖哪些产品、各占几成。设计相同的产品共用一套设施，份额随时可调，明天起生效。</Term></h2></div>
    {f.allocation.map(a => <AllocationGood key={a.good} f={f} a={a} />)}
  </section>;
}

/** 品牌: the company's brands, their products; new brand and rename for the operator. */
export function BrandsPanel({ c }: { c: Company }) {
  const openModal = useUIStore(s => s.openModal);
  const [creating, setCreating] = useState(false);
  const [name, setName] = useState('');
  const [renaming, setRenaming] = useState('');
  const [rename, setRename] = useState('');
  const brands = c.brands ?? [];
  if (!brands.length && !c.canPrice) return null;
  return <section className="panel-block brands-panel">
    <div className="block-head"><h2><Term name="品牌">品牌承载口碑：知名度与忠诚度按集散区、按品牌分开记。同一家公司的几个品牌共用工厂、专利和经验曲线；新品牌在各地都从零开始，但不会拖累老品牌。家庭先在品牌之间比，同一品牌的几款产品互相竞争。</Term></h2>
      <span className="block-note">{brands.length} / {c.maxBrands ?? 6} 个</span></div>
    {brands.length ? <ul className="brand-list">{brands.map(b => <li key={b.id}>
      {renaming === b.id ? <span className="inline-form"><label>新名称<input value={rename} onChange={e => setRename(e.target.value)} /></label>
        <button className="link" disabled={!rename.trim()} onClick={() => { command('brand.rename', b.id, { name: rename.trim() }); setRenaming(''); }}>确定</button>
        <button className="link" onClick={() => setRenaming('')}>取消</button></span>
        : <><button className="link name" onClick={() => openModal({ kind: 'product', subject: 'brand', id: b.id })}>{b.name}</button>
          <em>{b.products.map(p => p.name).join('、') || '还没有产品'}</em>
          {c.canPrice && <button className="link" onClick={() => { setRenaming(b.id); setRename(b.name); }}>改名</button>}</>}
    </li>)}</ul> : <p className="quiet">这家公司还没有品牌：同质商品不需要品牌。</p>}
    {c.canPrice && (creating
      ? <div className="inline-form"><label>品牌名称<input value={name} onChange={e => setName(e.target.value)} /></label>
          <button className="link" disabled={!name.trim()} onClick={() => { command('brand.create', c.id, { name: name.trim() }); setName(''); setCreating(false); }}>建立</button>
          <button className="link" onClick={() => setCreating(false)}>取消</button></div>
      : brands.length < (c.maxBrands ?? 6) && <button className="link" onClick={() => setCreating(true)}>新建品牌</button>)}
  </section>;
}

const NewBrand = '__new';

/** A list slot (a train's wagons): each known kind with its pieces; min..max pieces in all. */
function ListComponent({ component, pieces, onChange }: { component: DesignComponent; pieces: Record<string, number>; onChange: (id: string, n: number) => void }) {
  const total = component.techs.reduce((s, t) => s + (pieces[t.id] ?? 0), 0);
  const min = component.min ?? 1, max = component.max ?? 99;
  return <div className="component list-component">
    <span className="component-name">{component.name}<em className={total < min || total > max ? 'quote-reason' : ''}> 共 {total} 件（{min}–{max}）</em></span>
    <ul className="piece-list">{component.techs.map(t => {
      const n = pieces[t.id] ?? 0;
      return <li key={t.id} className={t.known ? '' : 'unknown'}>
        <span className="piece-name"><b>{t.name}</b><em>{t.known ? t.effects : '未掌握'}{t.cost ? ` · 每件 ${price(t.cost)}` : ''}</em></span>
        <span className="share-stepper">
          <button aria-label="减少" disabled={!t.known || n <= 0} onClick={() => onChange(t.id, n - 1)}>−</button><b>{n}</b>
          <button aria-label="增加" disabled={!t.known || total >= max} onClick={() => onChange(t.id, n + 1)}>+</button>
        </span>
      </li>;
    })}</ul>
  </div>;
}

/** 车船的设计：speed, room by cargo class, crew, life, fuel; how close it runs to its constraints; what one takes to build. */
function VehicleDesign({ d }: { d: DesignQuote }) {
  const v = d.vehicle;
  if (!v && !d.materials?.length) return null;
  return <div className="vehicle-design">
    {v && <dl className="vehicle-figures">
      <dt>速度</dt><dd>{v.kmPerDay} 公里／天</dd>
      <dt>载重</dt><dd>{v.room?.length ? v.room.map(r => r.seats !== undefined ? `${r.class} ${r.seats.toFixed(0)} 座` : `${r.class} ${r.tons.toFixed(0)} 吨`).join(' · ') : `${v.payload.toFixed(0)} 吨`}</dd>
      <dt>乘员</dt><dd>{v.crew} 人</dd>
      <dt>寿命</dt><dd>{v.lifeYears.toFixed(0)} 年</dd>
      {!!v.fuel.length && <><dt>燃料</dt><dd>{v.fuel.map(f => `${f.good} ${f.tonsPerDay < 1 ? f.tonsPerDay.toFixed(3) : f.tonsPerDay.toFixed(1)} 吨／天`).join(' · ')}</dd></>}
    </dl>}
    {!!v?.constraints.length && <div className="skill-list">{v.constraints.map(c => <div key={c.name} className={c.used > c.max ? 'short' : ''}>
      <div className="skill-head"><span>{c.name}</span><b>{c.tons ? `${c.used.toFixed(1)} / ${c.max.toFixed(1)} 吨` : `${c.used} / ${c.max}`}</b></div>
      <div className="skill-track"><span style={{ width: `${Math.min(100, c.max > 0 ? (c.used / c.max) * 100 : 100)}%` }} /></div>
    </div>)}</div>}
    {!!d.materials?.length && <p className="block-note">每{d.worth ? `件（部件值 ${pounds(d.worth)}）` : '件'}用料：{d.materials.map(m => `${m.good} ${m.perUnit < 10 ? m.perUnit.toFixed(2) : m.perUnit.toFixed(0)} ${m.unit}`).join('、')}{d.work ? ` · 工 ${pounds(d.work)}` : ''}</p>}
  </div>;
}

/** 新产品设计: a good, a base, one known tech per component; name, brand, price; the engineering quote live. */
export function ProductDesigner({ c, onDone }: { c: Company; onDone: () => void }) {
  const goods = c.designable ?? [];
  const [goodId, setGoodId] = useState(goods[0]?.id ?? '');
  const good = goods.find(g => g.id === goodId);
  const [baseId, setBaseId] = useState(good?.bases[0]?.id ?? '');
  const base = good?.bases.find(b => b.id === baseId);
  const [techs, setTechs] = useState<Record<string, string>>({});
  const [pieces, setPieces] = useState<Record<string, number>>({});
  // 工序形态：each stage's way ('' = an optional stage not done).
  const [stages, setStages] = useState<Record<string, string>>({});
  const [name, setName] = useState('');
  const [brand, setBrand] = useState('');
  const [newBrand, setNewBrand] = useState('');
  const [priceText, setPriceText] = useState('');
  useEffect(() => { setBaseId(goods.find(g => g.id === goodId)?.bases[0]?.id ?? ''); }, [goodId]); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => {
    const next: Record<string, string> = {};
    const nextPieces: Record<string, number> = {};
    for (const component of good?.components ?? []) {
      if (component.list) {
        // The base's wagons, else the default kind in its default number.
        const fromBase = component.techs.filter(t => base?.techs.includes(t.id));
        if (fromBase.length) for (const t of fromBase) nextPieces[t.id] = base?.counts?.[t.id] ?? 1;
        else { const d = component.techs.find(t => t.default) ?? component.techs[0]; if (d) nextPieces[d.id] = component.defaultCount ?? 1; }
        continue;
      }
      const fromBase = component.techs.find(t => base?.techs.includes(t.id));
      next[component.id] = (fromBase ?? component.techs.find(t => t.default) ?? component.techs[0])?.id ?? '';
    }
    setTechs(next);
    setPieces(nextPieces);
    setStages(chosenOf(good?.stages ?? [], base?.techs ?? []));
    setBrand(base?.brandId || c.brands?.[0]?.id || '');
  }, [goodId, baseId]); // eslint-disable-line react-hooks/exhaustive-deps
  const staged = !!good?.stages?.length;
  const listed = (good?.components ?? []).filter(component => component.list).flatMap(component => component.techs.filter(t => (pieces[t.id] ?? 0) > 0).map(t => t.id));
  const chosen = staged ? chainOf(good!.stages!, stages)
    : [...(good?.components ?? []).filter(component => !component.list).map(component => techs[component.id]), ...listed].filter(Boolean).join(',');
  const counts = listed.map(id => `${id}:${pieces[id]}`).join(',');
  const reply = useQuote(good ? { kind: 'design', company: c.id, good: goodId, base: baseId, techs: chosen, counts } : null, `${chosen}|${counts}`);
  const q = reply?.quote;
  const d = reply?.design;
  if (!goods.length) return <section className="panel-block designer"><p className="quiet">这家公司的业务没有可以设计的商品。</p></section>;
  const brandOk = !good?.branded || (brand === NewBrand ? !!newBrand.trim() : !!brand);
  const ok = !!q?.ok && !!name.trim() && brandOk;
  const submit = () => {
    command('product.design', c.id, { good: goodId, base: baseId, techs: chosen, counts, name: name.trim(), brand: brand === NewBrand ? '' : brand,
      newBrand: brand === NewBrand ? newBrand.trim() : '', price: priceText });
    onDone();
  };
  return <section className="panel-block designer">
    <div className="block-head"><h2><Term name="新产品设计">设计新产品只是把已掌握的部件技术组合起来：新用上的部件技术各付基准研发费的一成半作工程验证费，六周后可以投产；贵的是部件研发。与现有产品设计完全相同的，不用验证、立即可投产，还能与它共用产线——这是给同一种货开第二个品牌的办法。米这类工序形态的商品，设计就是一道道工序选哪种做法：经过清理、蒸谷、抛光、分级的米已经是另一种商品了。</Term></h2>
      <button className="link" onClick={onDone}>收起</button></div>
    <div className="modal-grid two">
      <div className="modal-column">
        <div className="form-grid">
          <div className="field"><span>商品</span><Select value={goodId} onChange={setGoodId} options={goods.map(g => ({ value: g.id, label: g.name }))} ariaLabel="商品" /></div>
          <div className="field"><span>底版</span>{good?.bases.length
            ? <Select value={baseId} onChange={setBaseId} options={good.bases.map(b => ({ value: b.id, label: b.name }))} ariaLabel="底版" />
            : <em>从默认设计开始</em>}</div>
        </div>
        {staged && <StageEditor slots={good!.stages!} chosen={stages} onChange={(stage, way) => setStages(s => ({ ...s, [stage]: way }))} unit={good!.unit} />}
        <div className="component-list">{(good?.components ?? []).map(component => {
          if (component.list) return <ListComponent key={component.id} component={component} pieces={pieces}
            onChange={(id, n) => setPieces(p => ({ ...p, [id]: Math.max(0, n) }))} />;
          const selected = component.techs.find(t => t.id === techs[component.id]);
          return <div key={component.id} className="component">
            <span className="component-name">{component.name}</span>
            <Select value={techs[component.id] ?? ''} onChange={v => setTechs(t => ({ ...t, [component.id]: v }))} minWidth={200} ariaLabel={component.name}
              options={component.techs.map(t => ({ value: t.id, label: t.known ? t.name : `${t.name} · 未掌握`, disabled: !t.known }))} />
            {selected && <em>{selected.effects}</em>}
          </div>;
        })}</div>
      </div>
      <div className="modal-column">
        {!!d?.attributes.length && <div className="skill-list">{d.attributes.map(a => <div key={a.id} className={a.value < a.base ? 'short' : ''}>
          <div className="skill-head"><span>{a.name}</span><b>{a.value}{a.value !== a.base && <em> {a.value > a.base ? '+' : '−'}{Math.abs(a.value - a.base)}（底版 {a.base}）</em>}</b></div>
          <div className="skill-track"><span style={{ width: `${a.value}%` }} /><i className="mean" style={{ left: `${a.base}%` }} /></div>
        </div>)}</div>}
        {d && <VehicleDesign d={d} />}
        <p className={`design-quote ${q && !q.ok ? 'quote-reason' : ''}`}>{!q ? '正在报价…' : !q.ok ? q.reason
          : d?.same ? `与「${d.sameName}」设计相同：不用工程验证，立即可投产，可以与它共用产线。`
          : q.days <= 0 ? `没有新用上的${staged ? '做法' : '部件技术'}：不用工程验证，立即可投产 · 继承底版经验 ${percent(d?.similarity ?? 0)}；要上产线须换产`
          : `工程验证 ${pounds(q.engineering)} · ${Math.max(1, Math.round(q.days / 7))} 周后可投产 · 继承底版经验 ${percent(d?.similarity ?? 0)}`}</p>
        <div className="form-grid">
          <label>产品名称<input value={name} onChange={e => setName(e.target.value)} /></label>
          {good?.branded && <div className="field"><span>品牌</span>
            <Select value={brand} onChange={setBrand} ariaLabel="品牌" options={[...(c.brands ?? []).map(b => ({ value: b.id, label: b.name })), { value: NewBrand, label: '新品牌…' }]} />
            {brand === NewBrand && <label>新品牌名称<input value={newBrand} onChange={e => setNewBrand(e.target.value)} /></label>}</div>}
          <div className="field"><span>出厂价（留空：{d ? price(d.price) : '与底版相同'}）</span><MoneyInput value={priceText} onChange={setPriceText} width={80} /></div>
        </div>
        <div className="confirm-row"><button className="action primary" disabled={!ok} onClick={submit}>{q?.ok && q.engineering > 0 ? `付 ${pounds(q.engineering)} 开始设计` : '建立产品'}</button>
          {!name.trim() && <span className="quote-reason">先起个名字。</span>}</div>
      </div>
    </div>
  </section>;
}

/** 改型的报价，一句话：工程验证、机器与安装、多久。 */
function redesignText(q: Quote) {
  if (q.kind === 'stage') return '不用工程验证，也不用买机器：马上生效，各条线照常生产。';
  const parts = [!q.engineeringDays ? '没有新用上的做法，不用工程验证'
    : q.engineering > 0 ? `工程验证 ${pounds(q.engineering)}、${Math.round(q.engineeringDays / 7)} 周` : `工程验证 ${Math.round(q.engineeringDays / 7)} 周（手工做法不收验证费）`];
  if (q.machines > 0) parts.push(`机器 ${pounds(q.machines)}`);
  if (q.install > 0) parts.push(`安装 ${pounds(q.install)}`);
  parts.push(`约 ${Math.max(1, Math.round(q.days / 7))} 周完工`);
  if (q.crafts?.length) parts.push(`工人手艺 ${craftsText(q)}`);
  return parts.join(' · ');
}

/** 改型（Processes.md 第 4、9 节）：一道道改做法，报价按做它的各条线合计；缺机器的工厂停下来一起装，别的照常生产，完工那天换上新链。 */
function StageRedesign({ p, onDone }: { p: CompanyProduct; onDone: () => void }) {
  const slots = p.stages ?? [];
  const current = useMemo(() => chainOf(slots, chosenOf(slots, (p.chain?.stages ?? []).flatMap(s => s.way ? [s.way] : []))), [p.chain, slots]);
  const [chosen, setChosen] = useState(() => chosenOf(slots, current.split(',')));
  const techs = chainOf(slots, chosen);
  const changed = techs !== current;
  const reply = useQuote(changed ? { kind: 'redesign', product: p.productId, techs } : null, techs);
  const q = changed ? reply?.quote : undefined;
  const submit = () => { command('project.redesign', p.productId, { techs }); onDone(); };
  return <div className="stage-redesign">
    <StageEditor slots={slots} chosen={chosen} onChange={(stage, way) => setChosen(s => ({ ...s, [stage]: way }))} unit={p.unit ?? ''} />
    {!changed ? <p className="quiet">换一道工序的做法，或加上、去掉一道可选工序；做它的各条线一起换。</p>
      : !q ? <p className="quiet">正在报价…</p>
      : !q.ok ? <p className="quote-reason">{q.reason}</p>
      : <>
        <p className="design-quote">{redesignText(q)}</p>
        {!!q.stops?.length && <p className="block-note">停产安装：{q.stops.join('、')}</p>}
        {!!q.runOn?.length && <p className="block-note">照常生产、完工那天换上：{q.runOn.join('、')}</p>}
        {!!q.dropped?.length && <p className="quote-reason">离开这些线（设计不同了）：{q.dropped.join('、')}</p>}
        {q.orders && <p className="block-note">设备：{q.orders}</p>}
        <div className="confirm-row"><button className="action primary" onClick={submit}>{q.kind === 'stage' ? '马上改' : q.dueNow > 0 ? `付 ${pounds(q.dueNow)} 开始改型` : '开始改型'}</button>
          <button className="link" onClick={onDone}>取消</button></div>
      </>}
  </div>;
}

/** 工序形态的产品（Processes.md 第 9 节）：它的工序链（额定值）；经营者改工序就是改型，做它的各条线一起换。 */
export function ProductStages({ p, canOperate }: { p: CompanyProduct; canOperate: boolean }) {
  const [open, setOpen] = useState(false);
  if (!p.chain) return null;
  const unit = p.unit ?? '';
  return <div className="product-stages">
    <span className="product-design"><span>工序：{p.chain.stages.filter(s => s.present).map(s => `${s.name} ${s.wayName}`).join(' · ')}{p.attributes ? `（${p.attributes}）` : ''}</span>
      <span className="block-note">{chainSummary(p.chain, unit, true)}</span></span>
    {p.project && <ul className="project-list"><ProjectRow p={p.project} canOperate={canOperate} /></ul>}
    {canOperate && !p.project && !!p.stages?.length && p.ready !== false && <button className="link" onClick={() => setOpen(!open)}>{open ? '收起改型' : '改工序（改型）'}</button>}
    {open && <StageRedesign p={p} onDone={() => setOpen(false)} />}
  </div>;
}

/** One product's brand, name, engineering state and lines (company panel). */
export function ProductControls({ c, p }: { c: Company; p: CompanyProduct }) {
  const openModal = useUIStore(s => s.openModal);
  const [renaming, setRenaming] = useState(false);
  const [name, setName] = useState(p.product);
  return <div className="product-controls">
    {p.ready === false && <span className="engineering-note">工程验证中 · {p.readyDate} 可投产</span>}
    <span className="product-lines">{p.lines?.length
      ? <>产线：{p.lines.map((l, i) => <Fragment key={l.id}>{i > 0 && '、'}<button className="link" onClick={() => openModal({ kind: 'facility', id: l.id, line: l.line, tab: 'products' })}>{l.name}</button>{l.share < 1 ? ` ${percent(l.share)}` : ''}</Fragment>)}</>
      : <em>还没有产线在做它{p.ready === false ? '' : '：到设施的“产线分配”里并线或换产'}</em>}</span>
    {c.canPrice && p.branded && (c.brands?.length ?? 0) > 1 && <span className="brand-select"><span>品牌</span>
      <Select value={p.brandId ?? ''} onChange={v => v !== p.brandId && command('product.set_brand', p.productId, { brand: v })} ariaLabel="品牌" minWidth={120}
        options={(c.brands ?? []).map(b => ({ value: b.id, label: b.name }))} /></span>}
    {c.canPrice && (renaming
      ? <span className="inline-form"><label>新名称<input value={name} onChange={e => setName(e.target.value)} /></label>
          <button className="link" disabled={!name.trim()} onClick={() => { command('product.rename', p.productId, { name: name.trim() }); setRenaming(false); }}>确定</button>
          <button className="link" onClick={() => setRenaming(false)}>取消</button></span>
      : <button className="link" onClick={() => { setName(p.product); setRenaming(true); }}>改名</button>)}
  </div>;
}
