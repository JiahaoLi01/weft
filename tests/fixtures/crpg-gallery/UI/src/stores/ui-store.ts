import { create } from 'zustand';
import { onUECall } from '@/lib/ue-bridge';

/**
 * UI Stack Store — manages layered page rendering.
 *
 * Pages are stacked: bottom pages remain mounted (and visible if semi-transparent).
 * SubtitleOverlay is NOT in the stack — it's always mounted independently.
 *
 * Commands from C++:
 *   CallUI("pushUI",    { page: "settings" })   → push on top of current stack
 *   CallUI("popUI",     {})                     → remove top page
 *   CallUI("replaceUI", { page: "test-menu" })  → clear stack, set as only page
 *   CallUI("clearUI",   {})                     → clear entire stack (blank canvas)
 */

interface UIState {
  /** Page stack — bottom to top. All pages in stack are rendered simultaneously. */
  stack: string[];

  /** Push a page onto the stack. No-op if already on top. */
  push: (page: string) => void;

  /** Pop the top page. No-op if stack is empty. */
  pop: () => void;

  /** Clear stack and set a single page. */
  replace: (page: string) => void;

  /** Clear entire stack (blank canvas). */
  clear: () => void;

  /** Check if a page is anywhere in the stack. */
  has: (page: string) => boolean;

  /** Get the topmost page (or null if empty). */
  top: () => string | null;

  // Legacy compat
  currentPage: string;
  goBack: () => void;
}

export const useUIStore = create<UIState>((set, get) => ({
  stack: [],

  push: (page) => {
    const { stack } = get();
    // Don't push if already on top
    if (stack.length > 0 && stack[stack.length - 1] === page) return;
    // Remove if already in stack (move to top)
    const filtered = stack.filter(p => p !== page);
    set({ stack: [...filtered, page], currentPage: page });
  },

  pop: () => {
    const { stack } = get();
    if (stack.length === 0) return;
    const next = stack.slice(0, -1);
    set({ stack: next, currentPage: next[next.length - 1] ?? '' });
  },

  replace: (page) => {
    set({ stack: [page], currentPage: page });
  },

  clear: () => {
    set({ stack: [], currentPage: '' });
  },

  has: (page) => get().stack.includes(page),

  top: () => {
    const { stack } = get();
    return stack.length > 0 ? stack[stack.length - 1] : null;
  },

  // Legacy compat
  currentPage: '',
  goBack: () => get().pop(),
}));

// ===== Register UE→JS commands =====

onUECall('pushUI', (data: unknown) => {
  const { page } = data as { page: string };
  if (page) useUIStore.getState().push(page);
});

onUECall('popUI', () => {
  useUIStore.getState().pop();
});

onUECall('replaceUI', (data: unknown) => {
  const { page } = data as { page: string };
  if (page) useUIStore.getState().replace(page);
});

onUECall('clearUI', () => {
  useUIStore.getState().clear();
});
