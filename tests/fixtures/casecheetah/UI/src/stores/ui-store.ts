import { create } from 'zustand';
import { onUECall, sendToUE } from '@/lib/ue-bridge';
import { focusOn } from '@/lib/focus';
import type { DesignQuote, Quote, RailPlan, StageChain } from '@/types';

/**
 * The old panel requests. A company, a building (or one of its lines), a product or a brand is now a focus of the
 * business tree with its dossier open (HierarchyUI.md); a construction or a plot's competition is a workbench.
 */
export type ModalState =
  | { kind: 'company'; id: string; tab?: string }
  | { kind: 'facility'; id: string; line?: number; tab?: string }
  | { kind: 'construct'; plot: string; company?: string; type?: string }
  | { kind: 'product'; subject: 'product' | 'brand'; id: string; market?: string; tab?: string }
  | { kind: 'plot-market'; plot: string; need?: string }
  | { kind: 'tech-tree'; company: string; industry?: string }
  | null;
/** The facility markers: the focus's (a company's buildings, else the business tree's group) and everyone's on the plot open on
 *  the left, or none. Everyone's everywhere would bury the map's figures. */
export type MarkerMode = 'group' | 'off';
/** The rail's pages: the map, the group page, the settings. */
export type Panel = 'world' | 'group' | 'settings';
/** The dossier beside the business tree: open (with the tab, line or market to open at) or closed. */
export type DossierState = { tab?: string; line?: number; market?: string } | null;
/** The right dock's books: the business tree (经营), the goods (商品) or the world's places (世界). */
export type Dock = 'business' | 'goods' | 'world';
export type QuoteReply = { req: string; quote: Quote; canOperate: boolean; suppliers: { id: string; name: string }[]; methods?: { id: string; name: string; effects: string; known: boolean; default: boolean }[];
  products?: { id: string; name: string; good: string; default: boolean; ready?: boolean; readyDate?: string }[]; design?: DesignQuote; rail?: RailPlan;
  /** 工序形态的改型：新的工序链（额定值）。 */
  chain?: StageChain };

interface UIState {
  panel: Panel;
  setPanel: (p: Panel) => void;
  uiScale: number;
  setScale: (n: number) => void;
  /** The workbench open over the dossier (construction, a plot's competition). */
  modal: ModalState;
  /** Opens a company, building, product or brand as the focus with its dossier, or a workbench. */
  openModal: (m: ModalState) => void;
  /** Closes the workbench, else the dossier. */
  closeModal: () => void;
  dossier: DossierState;
  openDossier: (d?: DossierState) => void;
  closeDossier: () => void;
  dock: Dock;
  setDock: (d: Dock) => void;
  /** The settings dialog. */
  settings: boolean;
  setSettings: (open: boolean) => void;
  /** The place clicked last (plot, 集散区 or country): its archive docked on the left. */
  placeOpen: boolean;
  setPlaceOpen: (open: boolean) => void;
  /** The buildings of the tree row under the pointer (lit on the map). */
  preview: string[];
  setPreview: (ids: string[]) => void;
  /** 选址：the next plot clicked opens the construction panel for this company. */
  siting: { company: string; name: string } | null;
  setSiting: (s: { company: string; name: string } | null) => void;
  markerMode: MarkerMode;
  cycleMarkers: () => void;
  quotes: Record<string, QuoteReply>;
}

const readMarkerMode = (): MarkerMode => {
  try { return localStorage.getItem('bm.markers') === 'off' ? 'off' : 'group'; } catch { return 'group'; }
};

export const useUIStore = create<UIState>((set, get) => ({
  panel: 'world',
  setPanel: (panel) => set({ panel }),
  uiScale: 1,
  setScale: (uiScale) => {
    const root = document.documentElement;
    root.style.zoom = String(uiScale);
    // Media queries see the physical window. The large panels compact themselves by the zoomed (logical) height.
    const height = window.innerHeight / uiScale;
    root.classList.toggle('ui-short', height < 760);
    root.classList.toggle('ui-cramped', height < 600);
    set({ uiScale });
  },
  modal: null,
  openModal: (modal) => {
    // A node of the business tree: it becomes the focus, its dossier opens (the workbench over it closes).
    if (modal?.kind === 'company' || modal?.kind === 'facility' || modal?.kind === 'product') {
      if (get().modal?.kind === 'plot-market') sendToUE('open_plot_market', { plot: '' });
      if (get().modal?.kind === 'tech-tree') sendToUE('open_tech_tree', { company: '' });
      if (modal.kind === 'company') { focusOn('company', modal.id); set({ dossier: { tab: modal.tab } }); }
      else if (modal.kind === 'facility') {
        if (modal.line !== undefined) focusOn('line', `${modal.id}#${modal.line}`);
        else focusOn('facility', modal.id);
        set({ dossier: { tab: modal.tab, line: modal.line } });
      }
      else { focusOn(modal.subject, modal.id); set({ dossier: { tab: modal.tab, market: modal.market } }); }
      set({ modal: null, siting: null, dock: 'business' });
      return;
    }
    const previous = get().modal;
    if (previous?.kind === 'plot-market' && modal?.kind !== 'plot-market') sendToUE('open_plot_market', { plot: '' });
    if (modal?.kind === 'plot-market') sendToUE('open_plot_market', { plot: modal.plot });
    // 科技树（TechTree.md 第 7 节）：the engine builds the tree for the company the workbench looks through.
    if (previous?.kind === 'tech-tree' && modal?.kind !== 'tech-tree') sendToUE('open_tech_tree', { company: '' });
    if (modal?.kind === 'tech-tree') sendToUE('open_tech_tree', { company: modal.company });
    set({ modal, siting: null });
  },
  closeModal: () => {
    if (get().modal) {
      if (get().modal?.kind === 'plot-market') sendToUE('open_plot_market', { plot: '' });
      if (get().modal?.kind === 'tech-tree') sendToUE('open_tech_tree', { company: '' });
      set({ modal: null });
    }
    else set({ dossier: null });
  },
  dossier: null,
  openDossier: (dossier) => set({ dossier: dossier ?? {} }),
  closeDossier: () => set({ dossier: null }),
  dock: 'business',
  setDock: (dock) => set({ dock }),
  settings: false,
  setSettings: (settings) => set({ settings }),
  placeOpen: false,
  setPlaceOpen: (placeOpen) => set({ placeOpen }),
  preview: [],
  setPreview: (preview) => set(s => (s.preview.length === 0 && preview.length === 0 ? s : { preview })),
  siting: null,
  setSiting: (siting) => set({ siting }),
  markerMode: readMarkerMode(),
  cycleMarkers: () => {
    const next: MarkerMode = get().markerMode === 'group' ? 'off' : 'group';
    try { localStorage.setItem('bm.markers', next); } catch { /* per-viewer convenience only */ }
    set({ markerMode: next });
  },
  quotes: {},
}));

let nextQuote = 1;
/** Asks UE for a project quote (read-only); the reply arrives on setQuote under the returned id. */
export function requestQuote(request: Record<string, string>): string {
  const req = `q${nextQuote++}`;
  sendToUE('request_quote', { ...request, req });
  return req;
}
onUECall('setQuote', (data) => {
  const reply = data as QuoteReply;
  useUIStore.setState((s) => {
    // Keep only recent replies; panels read the id they asked for last.
    const keys = Object.keys(s.quotes);
    const quotes = keys.length > 40 ? Object.fromEntries(keys.slice(-20).map(k => [k, s.quotes[k]])) : { ...s.quotes };
    quotes[reply.req] = reply;
    return { quotes };
  });
});
