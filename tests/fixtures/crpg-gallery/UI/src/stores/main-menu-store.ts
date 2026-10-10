import { create } from 'zustand';

export interface MainMenuState {
  /** Whether the main menu is visible */
  visible: boolean;
  /** Whether the intro animation has played */
  activated: boolean;
  /** Whether the exit animation is playing */
  exiting: boolean;
  /** Currently focused button index (-1 = none) */
  focusedIndex: number;

  // Actions
  show: () => void;
  hide: () => void;
  /** Trigger exit animation: reverses entrance, then hides after duration */
  startExit: () => void;
  setActivated: (v: boolean) => void;
  setFocusedIndex: (i: number) => void;
}

export const useMainMenuStore = create<MainMenuState>((set) => ({
  visible: true,
  activated: false,
  exiting: false,
  focusedIndex: -1,

  show: () => set({ visible: true, activated: false, exiting: false }),
  hide: () => set({ visible: false, activated: false, exiting: false }),
  startExit: () => set({ exiting: true, activated: false }),
  setActivated: (activated) => set({ activated }),
  setFocusedIndex: (focusedIndex) => set({ focusedIndex }),
}));
