import { focusOn } from '@/lib/focus';
import { sendToUE } from '@/lib/ue-bridge';
import { count, price } from '@/lib/format';
import { useUIStore } from '@/stores/ui-store';
import { GoodArt } from './BusinessTree';
import type { GoodDossierView, GoodsIndexRow, World } from '@/types';

// 商品书册（HierarchyUI.md 第 8.1 节）：every good by its class, the world's output last week, and what the player's group does
// with it — 产 (makes it), 用 (uses it). Picking a good makes it the focus: the map shows its 供需, the card its figures, 档案
// its chain, makers, users and 集散区.

/** 1.2万 for the ledger's narrow column. */
const compact = (n: number) => n >= 10000 ? `${(n / 10000).toFixed(n >= 100000 ? 0 : 1)}万` : count(n);

function GoodCard({ d, row }: { d?: GoodDossierView; row: GoodsIndexRow }) {
  const openDossier = useUIStore(s => s.openDossier);
  const f = d?.figures;
  const figures: [string, string][] = [
    ['上周产量', f?.output === undefined ? '模型外' : `${count(f.output)} ${row.unit}`],
    ['上周卖出', f ? `${count(f.sold)} ${row.unit}` : '…'],
    ['货架均价', f?.average === undefined ? '—' : `${price(f.average)}／${row.unit}`],
    ['在售集散区', f ? `${f.markets} 个` : '…'],
  ];
  return <div className="focus-card">
    <dl>{figures.map(([label, value]) => <div key={label}><dt>{label}</dt><dd>{value}</dd></div>)}</dl>
    <div className="focus-actions"><button className="brush-button" onClick={() => openDossier()}>档案</button></div>
  </div>;
}

export function GoodsBook({ world }: { world: World }) {
  const index = world.goodsIndex;
  const focus = world.focus;
  const openDossier = useUIStore(s => s.openDossier);
  if (!index) return <p className="quiet tree-empty">正在读取商品…</p>;
  const classes = [...new Set(index.goods.map(g => g.category))];
  const here = focus?.kind === 'good' ? index.goods.find(g => g.id === focus.id) : undefined;
  return <div className="goods-book">
    <nav className="breadcrumbs" aria-label="焦点路径">
      <span className="history" role="group" aria-label="焦点历史">
        <button disabled={!focus?.canBack} aria-label="回到上一个焦点（Alt+←）" title="Alt+←" onClick={() => sendToUE('navigate', { dir: 'back' })}>‹</button>
        <button disabled={!focus?.canForward} aria-label="前进（Alt+→）" title="Alt+→" onClick={() => sendToUE('navigate', { dir: 'forward' })}>›</button>
      </span>
      <span>商品{here && <><i>›</i><button className="current">{here.name}</button></>}</span>
    </nav>
    {classes.map(c => <section key={c} className="goods-class">
      <h3 className="tree-section">{c}</h3>
      <ul className="ledger-tree">{index.goods.filter(g => g.category === c).map(g => {
        const selected = focus?.kind === 'good' && focus.id === g.id;
        return <li key={g.id} className={`tree-node good-node ${selected ? 'selected' : ''}`} data-node={`good:${g.id}`}>
          <div className="tree-row good-row" role="button" tabIndex={0} aria-current={selected} onClick={() => focusOn('good', g.id)}
            onDoubleClick={() => { focusOn('good', g.id); openDossier(); }} onKeyDown={e => { if (e.key === 'Enter') focusOn('good', g.id); }}>
            <GoodArt id={g.id} />
            <span className="tree-name">{g.name}<em>{g.unit}</em></span>
            {(g.make || g.use) && <span className="good-roles" aria-label={`${g.make ? '本集团做它' : ''}${g.make && g.use ? '，' : ''}${g.use ? '本集团用它' : ''}`}>
              {g.make && <i className="role make">产</i>}{g.use && <i className="role use">用</i>}</span>}
            <span className="tree-figure">{g.output === undefined ? '' : compact(g.output)}</span>
          </div>
          {selected && <GoodCard d={world.goodDossier?.id === g.id ? world.goodDossier : undefined} row={g} />}
        </li>;
      })}</ul>
    </section>)}
  </div>;
}
