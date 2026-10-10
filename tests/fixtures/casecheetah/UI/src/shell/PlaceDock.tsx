import type { ReactNode } from 'react';
import { getZoom } from '@/components/tooltip/TooltipSystem';
import { NineSlice, panelArt } from '@/components/NineSlice';
import { sendToUE } from '@/lib/ue-bridge';
import { useFloatingUIStore } from '@/stores/floating-ui-store';
import { useUIStore } from '@/stores/ui-store';
import type { World } from '@/types';

// 地块档案（左侧）：a click on the ground — a plot, a 集散区 or a country — opens the place's archive docked at the left edge, whatever
// the right dock shows, and the focus stays where it is. Docked, it keeps a brush edge on its right side only (UIArtDirection.md
// 第 37–41 行); a gold dot marks the place on the map.

/** The place clicked, marked on the map (UE projects the 'place' anchor). */
function PlacePin() {
  const pos = useFloatingUIStore(s => s.positions.place);
  useUIStore(s => s.uiScale);
  if (!pos || pos.behind || !pos.onScreen) return null;
  const z = getZoom();
  return <div className="place-anchor" style={{ left: pos.nx * window.innerWidth / z, top: pos.ny * window.innerHeight / z }}><i className="place-dot" aria-hidden="true" /></div>;
}

export function PlaceDock({ world, children }: { world: World; children: ReactNode }) {
  const setPlaceOpen = useUIStore(s => s.setPlaceOpen);
  const setDock = useUIStore(s => s.setDock);
  const type = world.selection?.type ?? 'province';
  const countryId = world.geography.countryId;
  // Into the world book at the place: the tree opened to its plot and 集散区 (a country's dossier for a country).
  const locate = () => { if (type === 'country' && countryId) sendToUE('inspect_country', { id: countryId }); setDock('world'); };
  return <>
    <PlacePin />
    <aside className="left-dock" aria-label="地块档案">
      <NineSlice art={panelArt.dockLeft} className="left-dock-surface">
        <div className="left-dock-tools">
          <button className="link" onClick={locate}>{type === 'country' ? '国家档案' : '看这个集散区'}</button>
          <button className="place-close" aria-label="收起档案" onClick={() => setPlaceOpen(false)}>×</button>
        </div>
        <div className="dock-scroll">{children}</div>
      </NineSlice>
    </aside>
  </>;
}
