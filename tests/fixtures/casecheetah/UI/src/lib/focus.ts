import { sendToUE } from './ue-bridge';
import type { TreeKind } from '@/types';

// 焦点（HierarchyUI.md 第 4 节）：the page picks a node of the business tree; the engine moves the map to it. The map view a
// player last chose at each level is remembered on this device and used when they come back to that level.

const viewKey = (kind: string) => `bm.view.${kind}`;

export function rememberedView(kind: string): string {
  try { return localStorage.getItem(viewKey(kind)) ?? ''; } catch { return ''; }
}

/** Looks at a node of the business tree (kind and id; a line's id is its key 设施#线号), at a given map view or the one last
 *  chosen at its level. */
export function focusOn(kind: TreeKind, id: string, view?: string) {
  sendToUE('focus', { kind, id, view: view ?? rememberedView(kind) });
}

/** Picks a map view of the focus, and remembers it for its level. */
export function pickView(kind: TreeKind, view: string) {
  try { localStorage.setItem(viewKey(kind), view); } catch { /* a per-viewer convenience only */ }
  sendToUE('map_view', { view });
}
