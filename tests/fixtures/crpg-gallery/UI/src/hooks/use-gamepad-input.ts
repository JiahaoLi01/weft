import { useEffect } from 'react';
import { onUECall } from '@/lib/ue-bridge';

/**
 * Gamepad → keyboard adapter.
 *
 * Polls gamepad state and synthesizes keyboard events for the buttons we
 * care about (D-pad, left stick, A/B, shoulders). The spatial-navigation
 * runtime listens for the same keyboard events that an actual keyboard
 * would fire, so this hook keeps gamepad as a thin "input adapter" —
 * nothing in the rest of the UI needs to know whether the player is using
 * sticks, D-pad, or arrow keys.
 *
 * ── Two input sources, one logical model ──────────────────────────────
 *
 *  1. Browser / dev mode — `navigator.getGamepads()` returns OS-level
 *     gamepad state. There's no event-driven gamepad API, so we have to
 *     poll on each animation frame.
 *
 *  2. Game runtime (Ultralight inside UE) — the embedded browser cannot
 *     see hardware gamepads (UE captures input above the Ultralight
 *     layer). C++ owns an EnhancedInput Mapping Context dedicated to UI
 *     navigation (`UWebUIInputController` + IMC_UINavigation) that's
 *     gated on the JS UI-stack's top page; while the IMC is active, its
 *     input actions trigger callbacks that forward SEMANTIC events here:
 *
 *       window.__ue_dispatch('uiInput', {
 *         action: 'navigate' | 'confirm' | 'cancel' | 'tabPrev' | 'tabNext',
 *         phase:  'started' | 'triggered' | 'completed',
 *         x?: -1 | 0 | 1,    // navigate only
 *         y?: -1 | 0 | 1,    // navigate only
 *       })
 *
 *     C++ does the physical-button → semantic-action mapping (via the
 *     EnhancedInput IMC), so the JS side never has to know whether the
 *     player is on Xbox / PlayStation / Switch / keyboard. Rebinds happen
 *     entirely in UE — this hook is platform-agnostic.
 *
 * Both sources reduce to the same dispatched keyboard events that the
 * spatial-navigation runtime listens for; downstream code can't tell the
 * difference between a hardware gamepad button, a UE-forwarded action,
 * and a real keyboard arrow key.
 *
 * ── Mapping (Standard Gamepad layout, the W3C-mandated common shape) ──
 *
 *   Buttons 12 / 13 / 14 / 15 → ArrowUp / Down / Left / Right
 *   Button 0 (A / Cross)      → Enter
 *   Button 1 (B / Circle)     → Escape
 *   Left stick                → ArrowUp/Down/Left/Right (when past deadzone)
 *   Buttons 4 / 5 (LB / RB)   → PageUp / PageDown   (tab cycling)
 *
 * ── Press / repeat semantics ──────────────────────────────────────────
 *
 *  - First press fires immediately.
 *  - Held buttons fire again after `INITIAL_DELAY_MS` then every
 *    `REPEAT_INTERVAL_MS` thereafter. Repeat cadence is driven by whichever
 *    poll trigger is active — UE-event in production, RAF in browser.
 */

const DEADZONE = 0.5;
const INITIAL_DELAY_MS = 450;
const REPEAT_INTERVAL_MS = 90;

interface ButtonState {
  /** Time of the most recent press transition (rising edge). */
  pressedAt: number | null;
  /** Time we last fired a synthetic key event for this input. */
  lastFiredAt: number;
}

type LogicalKey =
  | 'ArrowUp' | 'ArrowDown' | 'ArrowLeft' | 'ArrowRight'
  | 'Enter' | 'Escape'
  | 'PageUp' | 'PageDown';

const KEYS: readonly LogicalKey[] = [
  'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight',
  'Enter', 'Escape', 'PageUp', 'PageDown',
];

/** Standard-mapping button index → logical key. */
const BUTTON_MAP: Record<number, LogicalKey> = {
  0: 'Enter',     // A / Cross
  1: 'Escape',    // B / Circle
  4: 'PageUp',    // LB
  5: 'PageDown',  // RB
  12: 'ArrowUp',
  13: 'ArrowDown',
  14: 'ArrowLeft',
  15: 'ArrowRight',
};

type ButtonsLike = readonly (boolean | { pressed: boolean })[];
type AxesLike = readonly number[];

function aggregatePad(
  buttons: ButtonsLike | undefined,
  axes: AxesLike | undefined,
  held: Set<LogicalKey>,
): void {
  if (buttons) {
    for (const [idxStr, logical] of Object.entries(BUTTON_MAP)) {
      const b = buttons[Number(idxStr)];
      const pressed = typeof b === 'boolean' ? b : b?.pressed;
      if (pressed) held.add(logical);
    }
  }
  if (axes) {
    const lx = axes[0] ?? 0;
    const ly = axes[1] ?? 0;
    if (Math.abs(lx) > DEADZONE || Math.abs(ly) > DEADZONE) {
      if (Math.abs(lx) > Math.abs(ly)) {
        held.add(lx > 0 ? 'ArrowRight' : 'ArrowLeft');
      } else {
        held.add(ly > 0 ? 'ArrowDown' : 'ArrowUp');
      }
    }
  }
}

function dispatchKey(key: LogicalKey): void {
  // keydown is what the spatial-navigation lib (and most app code) listens
  // for. We also dispatch a paired keyup so React + native handlers that
  // care about up edges stay balanced.
  const target = document.activeElement instanceof HTMLElement
    ? document.activeElement
    : document.body;
  target.dispatchEvent(new KeyboardEvent('keydown', { key, bubbles: true }));
  target.dispatchEvent(new KeyboardEvent('keyup',   { key, bubbles: true }));
}

/** Map a UE-forwarded semantic action to a logical key. The "navigate"
 *  action carries direction; everything else is a single-key press. */
function semanticActionToKey(
  action: string,
  x: number,
  y: number,
): LogicalKey | null {
  switch (action) {
    case 'navigate':
      // 8-direction quantization happens upstream (in C++). Here we just
      // pick one cardinal — diagonals are unusual for spatial nav and
      // C++ already collapsed them to dominant axis.
      if (x > 0) return 'ArrowRight';
      if (x < 0) return 'ArrowLeft';
      if (y > 0) return 'ArrowDown';
      if (y < 0) return 'ArrowUp';
      return null;
    case 'confirm': return 'Enter';
    case 'cancel':  return 'Escape';
    case 'tabPrev': return 'PageUp';
    case 'tabNext': return 'PageDown';
    default: return null;
  }
}

export function useGamepadInput(): void {
  useEffect(() => {
    const state: Record<LogicalKey, ButtonState> = KEYS.reduce(
      (acc, k) => { acc[k] = { pressedAt: null, lastFiredAt: 0 }; return acc; },
      {} as Record<LogicalKey, ButtonState>,
    );

    /** Hardware gamepad poll body — only used by the RAF loop below.
     *  UE-forwarded inputs go through the semantic 'uiInput' handler,
     *  which doesn't share this code path. */
    const pollHardware = () => {
      const now = performance.now();
      const held = new Set<LogicalKey>();
      if (typeof navigator.getGamepads === 'function') {
        for (const pad of navigator.getGamepads()) {
          if (pad) aggregatePad(pad.buttons, pad.axes, held);
        }
      }
      for (const key of KEYS) {
        const s = state[key];
        const isHeld = held.has(key);
        if (isHeld) {
          if (s.pressedAt === null) {
            s.pressedAt = now;
            s.lastFiredAt = now;
            dispatchKey(key);
          } else if (
            now - s.pressedAt >= INITIAL_DELAY_MS &&
            now - s.lastFiredAt >= REPEAT_INTERVAL_MS
          ) {
            s.lastFiredAt = now;
            dispatchKey(key);
          }
        } else if (s.pressedAt !== null) {
          s.pressedAt = null;
        }
      }
    };

    /** UE → JS semantic event handler. C++ calls this when an IA in the
     *  UI navigation IMC fires, AFTER the IMC itself is gated on the UI
     *  stack — so we don't need to think about gameplay context here.
     *
     *  Edge phases dispatch a single keyboard event (the rising edge);
     *  the spatial-nav lib's own throttle handles repeated arrow events
     *  when the player holds a direction long enough for C++ to emit
     *  more 'triggered' phases. */
    const unsubscribe = onUECall('uiInput', (data: unknown) => {
      const d = data as { action?: string; phase?: string; x?: number; y?: number };
      const action = d.action ?? '';
      const phase = d.phase ?? 'started';
      const key = semanticActionToKey(action, d.x ?? 0, d.y ?? 0);

      if (action === 'navigate') {
        // 'completed' on navigate means the stick / D-pad returned to
        // neutral — clear any held-key bookkeeping and don't dispatch.
        if (phase === 'completed' || !key) {
          for (const k of ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight'] as const) {
            state[k].pressedAt = null;
          }
          return;
        }
        // C++ already deduped to direction CHANGES, so each event here
        // is a fresh rising edge for some direction. Dispatch the key
        // and update the bookkeeping so a hardware-gamepad held in the
        // same direction won't double-fire.
        const now = performance.now();
        // Clear opposite axis to model "stick moved over": pressing right
        // should release any pending left, otherwise both stay 'held'.
        for (const k of ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight'] as const) {
          if (k !== key) state[k].pressedAt = null;
        }
        state[key].pressedAt = now;
        state[key].lastFiredAt = now;
        dispatchKey(key);
        return;
      }

      // tabPrev / tabNext are page-level "cycle category" gestures, not
      // spatial movement. The spatial-navigation lib only understands
      // arrow + enter, so dispatching PageUp/PageDown keys would land in
      // a default keymap that doesn't subscribe. Instead we emit a
      // CustomEvent that any tab-strip-owning page (Settings, future
      // inventory categories, ...) can subscribe to from React via
      // useEffect + window.addEventListener.
      if (action === 'tabPrev' || action === 'tabNext') {
        if (phase === 'started') {
          window.dispatchEvent(new CustomEvent(`ui:${action}`));
        }
        return;
      }

      // Digital actions: 'started' is the only edge we emit on; treat
      // everything else as a release.
      if (!key) return;
      if (phase === 'started') {
        const now = performance.now();
        state[key].pressedAt = now;
        state[key].lastFiredAt = now;
        dispatchKey(key);
      } else {
        state[key].pressedAt = null;
      }
    });

    // RAF loop for hardware gamepads. In production (Ultralight foreground)
    // RAF runs at vsync. In a backgrounded preview tab the loop stalls,
    // but the UE event path doesn't depend on it.
    let raf = 0;
    const tick = () => {
      raf = requestAnimationFrame(tick);
      pollHardware();
    };
    raf = requestAnimationFrame(tick);

    return () => {
      cancelAnimationFrame(raf);
      unsubscribe();
    };
  }, []);
}
