import { create } from 'zustand';

/**
 * UI input bindings + active input device, mirrored from UE.
 *
 * UE owns the EnhancedInput Mapping Context — it knows what keys are
 * actually bound to each UI action — and pushes the per-action key list to
 * us via `CallUI("uiInputBindings", {...})` when the input controller
 * activates. UE also observes which device the player is currently using
 * (gamepad vs keyboard/mouse) via the front-of-chain Slate input probe and
 * pushes transitions via `CallUI("uiInputDevice", {...})`.
 *
 * The Web UI consults this store to render input-hint pills with the keys
 * that match the player's hardware AND the project's actual rebinding-aware
 * configuration — never hardcodes "press A" or similar.
 *
 * ── Wire format ───────────────────────────────────────────────────────
 *
 *  uiInputBindings:
 *    {
 *      navigate: { gamepad: ["Gamepad_LeftX","Gamepad_LeftY","Gamepad_DPad_*"], keyboardMouse: ["W","A","S","D"] },
 *      confirm:  { gamepad: ["Gamepad_FaceButton_Bottom"], keyboardMouse: ["Enter"] },
 *      cancel:   { gamepad: ["Gamepad_FaceButton_Right"], keyboardMouse: ["Escape"] },
 *      tabPrev:  { gamepad: ["Gamepad_LeftShoulder"], keyboardMouse: ["Q"] },
 *      tabNext:  { gamepad: ["Gamepad_RightShoulder"], keyboardMouse: ["E"] },
 *    }
 *
 *  uiInputDevice: { device: "gamepad" | "keyboardMouse" }
 */

export type UIInputAction = 'navigate' | 'confirm' | 'cancel' | 'tabPrev' | 'tabNext';
export type UIInputDevice = 'gamepad' | 'keyboardMouse';

/**
 * Controller hardware family. The probe-derived `device` field tells us
 * "gamepad vs keyboard"; this `controllerFamily` tells us WHICH gamepad,
 * which selects the glyph dictionary (Xbox A vs PlayStation Cross vs
 * Switch B). Driven by C++-side HID VID/PID detection (see
 * `FWebUIControllerDetect::DetectActiveFamily`). Defaults to 'xbox'
 * because that's the right fallback for unknown XInput-shaped pads.
 */
export type ControllerFamily = 'xbox' | 'playstation' | 'switch' | 'generic' | 'unknown';

export interface UIActionBinding {
  /** Raw FKey names from UE (e.g. "Gamepad_FaceButton_Bottom", "Enter"). */
  gamepad: string[];
  keyboardMouse: string[];
}

export type UIInputBindings = Record<UIInputAction, UIActionBinding>;

interface UIInputState {
  bindings: UIInputBindings | null;
  device: UIInputDevice;
  controllerFamily: ControllerFamily;

  setBindings: (b: UIInputBindings) => void;
  setDevice: (d: UIInputDevice) => void;
  setControllerFamily: (f: ControllerFamily) => void;

  /** Helper: get the first key for an action under the current device. The
   *  primary use case (input-hint pills) only needs one canonical glyph;
   *  the array form is preserved for future "show all keys" affordances. */
  getKey: (action: UIInputAction) => string | null;
}

export const useUIInputStore = create<UIInputState>((set, get) => ({
  bindings: null,
  // Default to keyboard-mouse — most dev sessions start at the mouse, and
  // showing keyboard glyphs first means the hint is correct from frame 1
  // even before the probe has observed any input. The instant the player
  // touches the gamepad UE flips this.
  device: 'keyboardMouse',
  // 'xbox' is the safe default for any unknown XInput-style gamepad —
  // generic / no-name pads almost universally adopt the Xbox button
  // layout. C++ overrides this with the detected family on first
  // broadcast.
  controllerFamily: 'xbox',

  setBindings:         (b) => set({ bindings: b }),
  setDevice:           (d) => set({ device: d }),
  setControllerFamily: (f) => set({ controllerFamily: f }),

  getKey: (action) => {
    const { bindings, device } = get();
    if (!bindings) return null;
    const keys = bindings[action]?.[device];
    return keys && keys.length > 0 ? keys[0] : null;
  },
}));
