import { create } from 'zustand';

// The floating UI page's settings store: the one value its overlay reads, the UI scale, at the original store's
// default (stores/settings-store.ts reads it from localStorage, defaulting to 2). The page has no settings UI, and the
// original store's other state (settings schema, subtitles, locale sync) never reaches the overlay.
interface FloatingPageSettings {
  uiScale: number;
}

export const useSettingsStore = create<FloatingPageSettings>(() => ({ uiScale: 2 }));
