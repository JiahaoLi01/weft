import type { ControllerFamily } from '@/stores/ui-input-store';

/**
 * UE FKey name → short display label, dispatched by controller family.
 *
 * UE sends raw FKey names over the bridge ("Gamepad_FaceButton_Bottom",
 * "Gamepad_LeftShoulder", "Enter", ...). We map them to short legible
 * glyphs for input-hint pills. The choice of glyph depends on which
 * physical gamepad the player has connected — UE's gamepad layer
 * normalizes button positions but the LABELS players expect differ:
 *
 *      South face button → Xbox "A"   PlayStation "✕"   Switch "B"
 *      East face button  → Xbox "B"   PlayStation "◯"   Switch "A"
 *      West face button  → Xbox "X"   PlayStation "▢"   Switch "Y"
 *      North face button → Xbox "Y"   PlayStation "△"   Switch "X"
 *
 * (Switch is genuinely the worst — its physical layout matches Xbox but
 * its labels are SWAPPED on the A/B and X/Y axes, because Nintendo never
 * standardized with anyone.)
 *
 * The `controllerFamily` value comes from C++ HID detection — see
 * `FWebUIControllerDetect`. Keyboard glyphs are family-agnostic.
 */

// ── Family-specific gamepad glyphs ────────────────────────────────

const KEY_LABELS_XBOX: Record<string, string> = {
  'Gamepad_FaceButton_Bottom': 'A',
  'Gamepad_FaceButton_Right':  'B',
  'Gamepad_FaceButton_Left':   'X',
  'Gamepad_FaceButton_Top':    'Y',
  'Gamepad_LeftShoulder':  'LB',
  'Gamepad_RightShoulder': 'RB',
  'Gamepad_LeftTrigger':   'LT',
  'Gamepad_RightTrigger':  'RT',
  'Gamepad_LeftThumbstick':  'L3',
  'Gamepad_RightThumbstick': 'R3',
  'Gamepad_Special_Left':    'View',
  'Gamepad_Special_Right':   'Menu',
};

const KEY_LABELS_PLAYSTATION: Record<string, string> = {
  'Gamepad_FaceButton_Bottom': '✕',
  'Gamepad_FaceButton_Right':  '◯',
  'Gamepad_FaceButton_Left':   '▢',
  'Gamepad_FaceButton_Top':    '△',
  'Gamepad_LeftShoulder':  'L1',
  'Gamepad_RightShoulder': 'R1',
  'Gamepad_LeftTrigger':   'L2',
  'Gamepad_RightTrigger':  'R2',
  'Gamepad_LeftThumbstick':  'L3',
  'Gamepad_RightThumbstick': 'R3',
  'Gamepad_Special_Left':    'Share',
  'Gamepad_Special_Right':   'Options',
};

const KEY_LABELS_SWITCH: Record<string, string> = {
  // Switch face button labels are swapped vs. Xbox/PS on the A/B and
  // X/Y axes (south=B, east=A, west=Y, north=X). UE's `FaceButton_Bottom`
  // is geometric "south", which on Switch is labeled "B".
  'Gamepad_FaceButton_Bottom': 'B',
  'Gamepad_FaceButton_Right':  'A',
  'Gamepad_FaceButton_Left':   'Y',
  'Gamepad_FaceButton_Top':    'X',
  'Gamepad_LeftShoulder':  'L',
  'Gamepad_RightShoulder': 'R',
  'Gamepad_LeftTrigger':   'ZL',
  'Gamepad_RightTrigger':  'ZR',
  'Gamepad_LeftThumbstick':  'L-Stick',
  'Gamepad_RightThumbstick': 'R-Stick',
  'Gamepad_Special_Left':    '−',
  'Gamepad_Special_Right':   '+',
};

// Direction / axis labels are family-agnostic (every controller has
// a D-pad and analog sticks, just with different overall styling).
const KEY_LABELS_DIRECTIONAL: Record<string, string> = {
  'Gamepad_DPad_Up':    '↑',
  'Gamepad_DPad_Down':  '↓',
  'Gamepad_DPad_Left':  '←',
  'Gamepad_DPad_Right': '→',
  'Gamepad_LeftX':       'L-Stick X',
  'Gamepad_LeftY':       'L-Stick Y',
  'Gamepad_Left2D_Axis': 'L-Stick',
  'Gamepad_RightX':      'R-Stick X',
  'Gamepad_RightY':      'R-Stick Y',
};

// Keyboard keys — same for every controller family.
const KEY_LABELS_KEYBOARD: Record<string, string> = {
  'Enter':       '↵',
  'SpaceBar':    'Space',
  'Escape':      'Esc',
  'Up':          '↑',
  'Down':        '↓',
  'Left':        '←',
  'Right':       '→',
  'Tab':         'Tab',
  'BackSpace':   '⌫',
};

function pickFamilyTable(family: ControllerFamily): Record<string, string> {
  switch (family) {
    case 'playstation': return KEY_LABELS_PLAYSTATION;
    case 'switch':      return KEY_LABELS_SWITCH;
    // 'generic' / 'unknown' both fall through to Xbox labels — that's the
    // right answer for any XInput-style pad whose vendor we don't recognize.
    default:            return KEY_LABELS_XBOX;
  }
}

export function fkeyToGlyph(
  fkeyName: string | null | undefined,
  family: ControllerFamily = 'xbox',
): string {
  if (!fkeyName) return '';

  // Lookup order: family-specific table → directional/axis table →
  // keyboard table → fallback (strip Gamepad_ prefix).
  const familyTable = pickFamilyTable(family);
  return (
    familyTable[fkeyName] ??
    KEY_LABELS_DIRECTIONAL[fkeyName] ??
    KEY_LABELS_KEYBOARD[fkeyName] ??
    fkeyName.replace(/^Gamepad_/, '').replace(/_/g, ' ')
  );
}
