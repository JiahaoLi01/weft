import { useState } from 'react';
import { focusOn } from '@/lib/focus';
import { sendToUE } from '@/lib/ue-bridge';
import { useUIStore } from '@/stores/ui-store';
import { people } from '@/lib/format';
import type { World } from '@/types';

// 世界树（HierarchyUI.md 第 8.2 节）：世界 → 国家 → 集散区 → 地块. By default only the path to the place picked shows (its
// country, 集散区 and plot), so the archive below stays in view; a country opens to all its 集散区 (Britain's, with its colonies,
// are 39), the place's 集散区 to all its plots. Picking a country's name opens its dossier below; a 集散区 or a plot flies there
// and opens its archive. 列强: the other groups — picking one observes it in the business tree. Replaces the old 集散区索引.

export function WorldTree({ world }: { world: World }) {
  const tree = world.worldTree;
  const setDock = useUIStore(s => s.setDock);
  const [manual, setManual] = useState<Record<string, boolean>>({});
  const [everyone, setEveryone] = useState(false);
  if (!tree) return null;
  const here = world.geography.countryId ?? '';
  const market = world.geography.marketId ?? '';
  const isOpen = (id: string) => manual[id] ?? false;
  const path = world.placePath;
  const plot = path?.plot?.id ?? '';
  const inspected = world.countryArchive?.id;
  const toggle = (id: string) => setManual(m => ({ ...m, [id]: !isOpen(id) }));
  const markets = tree.countries.reduce((n, c) => n + c.markets.length, 0);
  // The country of the place first; the others behind a fold (about a hundred polities in 1844).
  const shown = everyone ? tree.countries : tree.countries.filter(c => c.id === here || manual[c.id]);
  return <section className="world-tree" aria-label="世界树">
    <ul className="ledger-tree">
      <li className="tree-node world-root"><div className="tree-row"><span className="tree-name">世界</span><em>{tree.countries.length} 个国家 · {markets} 个集散区</em></div></li>
      {shown.map(c => <li key={c.id} className={`tree-node country ${isOpen(c.id) ? 'open' : ''} ${c.id === here ? 'here' : ''} ${c.id === inspected ? 'selected' : ''}`}>
        <div className="tree-row country-row">
          <button className={`twist ${isOpen(c.id) ? 'open' : 'closed'}`} aria-label={isOpen(c.id) ? '收起集散区' : '展开集散区'} aria-expanded={isOpen(c.id)} onClick={() => toggle(c.id)} />
          <button className="tree-name" onClick={() => sendToUE('inspect_country', { id: c.id })}>{c.name}</button><em>{c.markets.length} 个集散区</em></div>
        {(isOpen(c.id) || c.id === here) && <ul className="tree-children">{(isOpen(c.id) ? c.markets : c.markets.filter(m => m.id === market)).map(m => {
          const plots = m.id === path?.market?.id ? path.market.plots : [];
          const openPlots = isOpen(`plots:${m.id}`);
          const shownPlots = openPlots ? plots : plots.filter(p => p.id === plot);
          return <li key={m.id} className={`tree-node market ${m.id === market && !inspected ? 'selected' : ''}`}>
            <div className="tree-row market-row">
              {plots.length > 0 ? <button className={`twist ${openPlots ? 'open' : 'closed'}`} aria-label={openPlots ? '收起地块' : '展开地块'} aria-expanded={openPlots} onClick={() => toggle(`plots:${m.id}`)} />
                : <i className="twist leaf" aria-hidden="true" />}
              <button className="tree-name" aria-current={m.id === market} onClick={() => sendToUE('focus_market', m)}>{m.name}</button><em>{m.plots} 块</em></div>
            {shownPlots.length > 0 && <ul className="tree-children">{shownPlots.map(p => <li key={p.id} className={`tree-node plot ${p.id === plot && !inspected ? 'selected' : ''}`}>
              <button className="tree-row plot-row" aria-current={p.id === plot} onClick={() => sendToUE('select_plot', { plot: p.id })}>
                <span className="tree-name">{p.name}</span>{p.population !== undefined && <em>{people(p.population)}</em>}</button></li>)}</ul>}
          </li>;
        })}</ul>}
      </li>)}
      <li className="tree-fold"><button className="link" onClick={() => setEveryone(!everyone)}>{everyone ? '只看这里' : `全部国家（${tree.countries.length}）`}</button></li>
      {!!tree.powers.length && <li className="tree-section">列强</li>}
      {tree.powers.map(p => <li key={p.id} className="tree-node power">
        <button className="tree-row power-row" onClick={() => { focusOn('group', p.id); setDock('business'); }}>
          <span className="tree-name">{p.name}</span>{p.home && <em>{p.home}</em>}</button></li>)}
    </ul>
  </section>;
}
