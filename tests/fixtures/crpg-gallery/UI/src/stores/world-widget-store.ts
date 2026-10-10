import { create } from 'zustand';
import { onUECall } from '@/lib/ue-bridge';

/**
 * World Widget Store — state for screen-projected UI elements attached to world actors.
 *
 * C++ side: `UWorldWidgetController` pushes an entire batch every tick via
 *   CallUI("__world_widget_batch", { widgets: [...] })
 *
 * Each widget is keyed by its C++ UObject UniqueID (stable for the anchor's lifetime).
 * Rendering is handled by `WorldWidgetOverlay`, which maps `type` → React component
 * via `config/world-widget-types.ts`.
 */

export interface WorldWidgetEntry {
  id: string;
  type: string;
  x: number;
  y: number;
  scale: number;
  visible: boolean;
  zOrder: number;
  distance: number;
  /**
   * Per-anchor payload. Values come from two sources on the C++ side:
   * - ViewModel FieldNotify properties (typed: number | boolean | string)
   * - Anchor WebData TMap<FName,FString> fallback (always string)
   * Consumer components should coerce via Number(...) / String(...).
   */
  data: Record<string, unknown>;
}

interface WorldWidgetState {
  widgets: WorldWidgetEntry[];
  setWidgets: (widgets: WorldWidgetEntry[]) => void;
}

export const useWorldWidgetStore = create<WorldWidgetState>((set) => ({
  widgets: [],
  setWidgets: (widgets) => set({ widgets }),
}));

// ===== UE → JS handler (registered at module load, same pattern as ui-store) =====

onUECall('__world_widget_batch', (data: unknown) => {
  const payload = data as { widgets?: WorldWidgetEntry[] };
  useWorldWidgetStore.getState().setWidgets(payload?.widgets ?? []);
});
