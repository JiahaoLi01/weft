import { create } from 'zustand';

// ===== Types =====

export interface SubtitleEntry {
  id: string;
  content: string;
  speaker: string;
  /** Sound description line (e.g. `[门关闭的声音]`) rather than spoken dialogue. */
  isSoundAssistant: boolean;
  /** Timestamp when this subtitle was added (for ordering / auto-remove). */
  addedAt: number;
  /**
   * Monotonic sequence number — drives CSS `order` so newer subtitles stack
   * below older ones WITHOUT moving DOM nodes (the pool keeps every slot at a
   * fixed DOM index; visual stacking is purely flex `order`). Kept small (a
   * per-session counter) so it stays well inside the CSS `<integer>` range,
   * unlike `addedAt` which would overflow int32.
   */
  seq: number;
  /**
   * Animation state. Drives the CSS transition in SubtitleOverlay.
   *
   * Is this REALLY needed? Yes. This isn't a cosmetic animation — it's
   * load-bearing for correctness. A pure instant show/hide leaves the last
   * frame frozen on the Ultralight-composited texture; a short CSS transition
   * keeps the paint pipeline active long enough for the final empty frame to
   * reach the display. (A `null` slot — see below — is the 'idle' state.)
   */
  state: 'entering' | 'visible' | 'exiting';
}

/**
 * One pool slot. `null` = idle (empty, `display:none`, collapsed). A non-null
 * slot holds the subtitle currently occupying that fixed DOM index.
 */
export type SubtitleSlot = SubtitleEntry | null;

/**
 * Furigana density — Japanese-only subtitle preference.
 *
 *  off — every <rt> hidden; kanji read on its own
 *  key — only key-word <rt> visible (author-marked with `!` syntax)
 *  all — every <rt> visible
 *
 * The setting is exposed in the Settings panel only when locale === 'ja'
 * (visibleWhen on the SettingDef). Stored regardless of locale so a
 * preference set in Japanese persists across language toggles.
 */
export type FuriganaMode = 'off' | 'key' | 'all';

export interface SubtitleSettings {
  /** Show speaker name prefix (e.g. "Aldric: Hello"). */
  showSpeakerName: boolean;
  /** Colorize speaker name. If false, speaker name is same color as content. */
  showSpeakerColor: boolean;
  /** Font size multiplier (1.0 = default). */
  fontScale: number;
  /** Background opacity for subtitle box (0–1). */
  bgOpacity: number;
  /** Max visible subtitles at once. Oldest removed when exceeded. */
  maxVisible: number;
  /** Furigana density (Japanese-only — see FuriganaMode docstring). */
  furiganaMode: FuriganaMode;
}

interface SubtitleState {
  /**
   * Fixed-length DOM pool. Slots are NEVER added or removed from this array —
   * `show`/`hide` only assign or null-out a slot. This is the whole point:
   * Ultralight's paint pipeline hitches on the DOM-node *removal* mutation
   * (the old design unmounted each `.subtitle-item` on exit, which dropped a
   * frame in-engine — see SubtitleOverlay header). With a fixed pool nothing
   * ever unmounts, so there's no removal to choke on, and the old
   * `subtitle-root-keepalive` masking hack is no longer needed.
   */
  slots: SubtitleSlot[];
  settings: SubtitleSettings;

  show: (data: { id: string; content: string; speaker: string; duration?: number; isSoundAssistant?: boolean }) => void;
  hide: (id: string) => void;
  hideAll: () => void;
  /** Internal: flip a slot's entry entering→visible (next-frame, see show). */
  promote: (id: string) => void;
  /** Internal: start a slot's exit transition. */
  startExit: (id: string) => void;
  /** Internal: return a slot to idle (null) once its exit transition has run. */
  clearSlot: (id: string) => void;
  updateSettings: (partial: Partial<SubtitleSettings>) => void;
  /** Reload settings from localStorage (called after Settings page apply). */
  reloadSettings: () => void;
}

// ===== Defaults =====

const DEFAULT_SETTINGS: SubtitleSettings = {
  showSpeakerName: true,
  showSpeakerColor: true,
  fontScale: 1.0,
  bgOpacity: 0.7,
  maxVisible: 3,
  furiganaMode: 'all',
};

function loadSettings(): SubtitleSettings {
  try {
    const saved = localStorage.getItem('subtitleSettings');
    if (saved) return { ...DEFAULT_SETTINGS, ...JSON.parse(saved) };
  } catch { /* ignore */ }
  return { ...DEFAULT_SETTINGS };
}

// ===== Pool sizing =====
//
// A fixed pool of always-mounted slots. Sized comfortably above any realistic
// `maxVisible` (cinematic subtitles are usually 1 at a time; 3 is the cap) plus
// headroom so an evicting subtitle can fade out in one slot while its
// replacement enters another. Idle slots are `display:none` — zero paint, zero
// layout — so an oversized pool is effectively free.
const POOL_SIZE = 8;

// ===== Animation timing =====
//
// Must stay in sync with the corresponding durations in styles/features/subtitle.css
// (`.subtitle-item[data-state="exiting"] .subtitle-glow` and `… .subtitle-text`).
const CSS_TRANSITION_MS = 200;    // exit fade-out duration (glow + text)
const ENTER_FRAME_DELAY_MS = 16;  // one vsync; lets initial opacity:0 paint land before flipping to visible
const EXIT_SAFETY_MS = 25;        // slack after the CSS transition before the slot goes idle

// Module-local monotonic sequence counter (drives CSS `order`). Lives outside
// the store state because it never needs to trigger a re-render on its own.
let seqCounter = 0;
const nextSeq = () => ++seqCounter;

// ===== Helpers (pure, operate on a slots array copy) =====

/** Index of the slot holding `id`, or -1. */
function slotIndexOf(slots: SubtitleSlot[], id: string): number {
  return slots.findIndex((s) => s !== null && s.id === id);
}

/** Index of the oldest slot matching `pred` (lowest addedAt), or -1. */
function oldestIndex(slots: SubtitleSlot[], pred: (e: SubtitleEntry) => boolean): number {
  let best = -1;
  let bestAt = Infinity;
  for (let i = 0; i < slots.length; i++) {
    const e = slots[i];
    if (e && pred(e) && e.addedAt < bestAt) {
      bestAt = e.addedAt;
      best = i;
    }
  }
  return best;
}

// ===== Store =====

export const useSubtitleStore = create<SubtitleState>((set, get) => ({
  slots: new Array<SubtitleSlot>(POOL_SIZE).fill(null),
  settings: loadSettings(),

  show: (data) => {
    const { settings } = get();
    const slots = [...get().slots];

    // 1. Same id already onscreen → reuse its slot in place (content update /
    //    timer reset). Keeps the same DOM index, so no churn.
    let idx = slotIndexOf(slots, data.id);

    if (idx < 0) {
      // 2. Enforce maxVisible — evict the oldest non-exiting subtitle (start
      //    its exit; it'll free its own slot when the transition finishes).
      const activeCount = slots.filter((s) => s !== null && s.state !== 'exiting').length;
      if (activeCount >= settings.maxVisible) {
        const evictIdx = oldestIndex(slots, (e) => e.state !== 'exiting');
        if (evictIdx >= 0) {
          const victim = slots[evictIdx]!;
          slots[evictIdx] = { ...victim, state: 'exiting' };
          // schedule the victim's slot to go idle after its fade
          const victimId = victim.id;
          setTimeout(() => get().clearSlot(victimId), CSS_TRANSITION_MS + EXIT_SAFETY_MS);
        }
      }

      // 3. Find a free (idle) slot for the newcomer. With POOL_SIZE headroom
      //    there is essentially always one; the fallback overwrites the oldest
      //    already-exiting slot rather than dropping the subtitle.
      idx = slots.findIndex((s) => s === null);
      if (idx < 0) idx = oldestIndex(slots, (e) => e.state === 'exiting');
      if (idx < 0) idx = 0; // last-resort: clobber slot 0 (pool exhausted — shouldn't happen)
    }

    slots[idx] = {
      id: data.id,
      content: data.content,
      speaker: data.speaker,
      isSoundAssistant: data.isSoundAssistant ?? false,
      addedAt: Date.now(),
      seq: nextSeq(),
      state: 'entering',
    };
    set({ slots });

    // Flip entering → visible on the next frame so opacity:0 paints once before
    // the CSS transition runs (otherwise React can batch the updates and skip
    // the initial opacity:0 render entirely).
    setTimeout(() => get().promote(data.id), ENTER_FRAME_DELAY_MS);

    // Auto-hide after duration (if provided).
    if (data.duration && data.duration > 0) {
      setTimeout(() => get().hide(data.id), data.duration * 1000);
    }
  },

  promote: (id) => {
    set((s) => {
      const i = slotIndexOf(s.slots, id);
      if (i < 0 || s.slots[i]!.state !== 'entering') return s;
      const slots = [...s.slots];
      slots[i] = { ...slots[i]!, state: 'visible' };
      return { slots };
    });
  },

  hide: (id) => {
    const i = slotIndexOf(get().slots, id);
    if (i < 0) return;
    const entry = get().slots[i]!;
    if (entry.state === 'exiting') return; // already leaving
    get().startExit(id);
  },

  hideAll: () => {
    set((s) => ({
      slots: s.slots.map((e) => (e && e.state !== 'exiting' ? { ...e, state: 'exiting' } : e)),
    }));
    // After the fade, return every exiting slot to idle. Guarded per-slot so a
    // slot reused for a new subtitle in the meantime is not wrongly cleared.
    const ids = get().slots.filter((e) => e && e.state === 'exiting').map((e) => e!.id);
    setTimeout(() => {
      for (const id of ids) get().clearSlot(id);
    }, CSS_TRANSITION_MS + EXIT_SAFETY_MS);
  },

  startExit: (id) => {
    set((s) => {
      const i = slotIndexOf(s.slots, id);
      if (i < 0) return s;
      const slots = [...s.slots];
      slots[i] = { ...slots[i]!, state: 'exiting' };
      return { slots };
    });
    setTimeout(() => get().clearSlot(id), CSS_TRANSITION_MS + EXIT_SAFETY_MS);
  },

  clearSlot: (id) => {
    set((s) => {
      const i = slotIndexOf(s.slots, id);
      // Only clear if the slot still holds THIS id AND it's still exiting —
      // guards against a slot reused for a newer subtitle before the timer fired.
      if (i < 0 || s.slots[i]!.state !== 'exiting') return s;
      const slots = [...s.slots];
      slots[i] = null; // back to idle — the DOM node stays mounted, just empties
      return { slots };
    });
  },

  updateSettings: (partial) => {
    set((s) => {
      const next = { ...s.settings, ...partial };
      localStorage.setItem('subtitleSettings', JSON.stringify(next));
      return { settings: next };
    });
  },

  reloadSettings: () => {
    set({ settings: loadSettings() });
  },
}));

// Dev-only: expose the store on window so a future stuck frame can be
// inspected in-place without a full RenderDoc capture:
//   > window.__subtitleStore.getState().slots     // what JS thinks is showing
//   > window.__subtitleStore.getState().hideAll() // force a clear and see if it actually clears
// If `slots` are all idle (null) but the screen still shows something → the bug
// is downstream of React (Ultralight paint / Slate texture cache).
// If a slot is still in 'exiting' state → the bug is here.
if (import.meta.env.DEV) {
  (window as unknown as { __subtitleStore?: typeof useSubtitleStore }).__subtitleStore = useSubtitleStore;
}
