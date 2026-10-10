import { create } from 'zustand';
import { sendToUE } from '@/lib/ue-bridge';
import { useSubtitleStore } from '@/stores/subtitle-store';
import { useI18nStore } from '@/stores/i18n-store';
import { LOCALE_LABELS, type Locale } from '@/i18n';
import type { LocaleKeys } from '@/i18n';
import {
  FONTS,
  FONT_SLOTS,
  applyFont,
  getFontLabel,
  getSavedFontId,
  type FontSlot,
} from '@/config/fonts';
import {
  MARKET_COLOR_IDS,
  loadMarketColorMode,
  saveMarketColorMode,
} from '@/config/market-colors';

// ===== Types =====

export interface SettingDef {
  type: 'scalar' | 'toggle' | 'discrete' | 'subtitle';
  value: number | boolean;
  label: string;
  description?: string;
  min?: number;
  max?: number;
  step?: number;
  options?: string[];
  /**
   * Per-option descriptions shown next to the option name in the detail
   * panel. Used to differentiate option meanings beyond their short label
   * (e.g. "Key words only — show readings only on words tagged by the
   * author"). Length must match `options`.
   */
  optionDescriptions?: string[];
  /**
   * For discrete settings: parallel array of stable id strings, one per option.
   * When present, the apply() side uses `ids[value]` instead of the raw numeric
   * index, letting us drive things like locale ("en", "zh-CN") and font id
   * ("noto-serif-sc") without leaking numeric indices to consumers.
   */
  ids?: string[];
  media?: { type: 'image' | 'video'; url: string };
  /**
   * i18n keys for label/description. When present, Settings.tsx translates
   * these at render time so the panel re-localizes live on language change,
   * without a schema reload. The plain `label`/`description` fields remain
   * as fallbacks (used for C++-provided settings that haven't been keyed).
   */
  labelKey?: LocaleKeys;
  descriptionKey?: LocaleKeys;
  /**
   * i18n keys parallel to `options` / `optionDescriptions`. When present,
   * Settings.tsx translates them at render time, fixing the "options are
   * frozen at schema-load time" problem for any setting that uses these
   * keys instead of pre-localized strings. The plain string arrays remain
   * as fallbacks for C++-provided options that aren't keyed yet.
   */
  optionsKey?: LocaleKeys[];
  optionDescriptionsKey?: LocaleKeys[];
  /**
   * Conditional visibility/interactivity based on another setting's value.
   * - visibleWhen: completely hide this setting when condition is false
   * - enabledWhen: show greyed out when condition is false
   */
  visibleWhen?: { key: string; equals: unknown };
  enabledWhen?: { key: string; equals: unknown };
  /**
   * When set, the detail panel renders a custom preview for this setting
   * instead of (or in addition to) the generic option list. Currently
   * supported: 'colorblind' — renders simulated color-blind palettes for
   * each option so players can compare modes side-by-side.
   */
  previewKind?: 'colorblind';
}

export interface SettingsCategory {
  label: string;
  /** i18n key for the category label; Settings.tsx translates at render time. */
  labelKey?: LocaleKeys;
  settings: Record<string, SettingDef>;
}

export type SettingsSchema = Record<string, SettingsCategory>;

interface SettingsState {
  schema: SettingsSchema | null;
  original: Record<string, unknown>;
  values: Record<string, unknown>;
  isDirty: boolean;

  /** Global UI scale (CSS zoom). Applied immediately, persisted on Apply. */
  uiScale: number;

  loadSchema: (schema: SettingsSchema) => void;
  setValue: (key: string, value: unknown) => void;
  apply: () => void;
  cancel: () => void;
}

// ===== Constants =====

/**
 * Keys that live entirely on the web side — never forwarded to C++ in the
 * settings_apply payload. Kept alongside the other constants so it's obvious
 * when new client-only settings are introduced.
 */
const CLIENT_ONLY_KEYS = new Set<string>([
  // Interface (injected)
  'language', 'uiFont', 'displayFont', 'subtitleFont',
  // Accessibility — subtitle (injected)
  'showSpeakerName', 'showSpeakerColor', 'subtitleFontScale', 'subtitleBgOpacity',
  'furiganaMode',
  '_subtitleHeader',
  // Accessibility — market (injected)
  'marketColorMode', '_marketHeader',
  // Client-side UI scale
  'uiScale',
]);

/** Locale id order — index maps to the `discrete` option index. */
const LOCALE_IDS = Object.keys(LOCALE_LABELS) as Locale[];

// ===== Helpers =====

/** Flatten schema into a key→value map */
function extractValues(schema: SettingsSchema): Record<string, unknown> {
  const vals: Record<string, unknown> = {};
  for (const cat of Object.values(schema)) {
    for (const [key, def] of Object.entries(cat.settings)) {
      vals[key] = def.value;
    }
  }
  return vals;
}

/** Furigana modes — index in this array maps to the discrete option index. */
const FURIGANA_IDS = ['off', 'key', 'all'] as const;
type FuriganaMode = typeof FURIGANA_IDS[number];

/** Load saved subtitle settings from localStorage */
function loadSubtitleSettings() {
  const defaults = {
    showSpeakerName: true,
    showSpeakerColor: true,
    fontScale: 1.0,
    bgOpacity: 0.7,
    furiganaMode: 'all' as FuriganaMode,
  };
  try {
    const saved = localStorage.getItem('subtitleSettings');
    if (saved) return { ...defaults, ...JSON.parse(saved) };
  } catch { /* ignore */ }
  return defaults;
}

/** Sync subtitle-related values to subtitle store's localStorage */
function syncSubtitleSettings(values: Record<string, unknown>) {
  const subtitleKeys = ['showSpeakerName', 'showSpeakerColor', 'subtitleFontScale', 'subtitleBgOpacity', 'furiganaMode'];
  const hasSubtitleChange = subtitleKeys.some(k => k in values);
  if (!hasSubtitleChange) return;

  const current = loadSubtitleSettings();
  if ('showSpeakerName' in values) current.showSpeakerName = values.showSpeakerName as boolean;
  if ('showSpeakerColor' in values) current.showSpeakerColor = values.showSpeakerColor as boolean;
  if ('subtitleFontScale' in values) current.fontScale = values.subtitleFontScale as number;
  if ('subtitleBgOpacity' in values) current.bgOpacity = values.subtitleBgOpacity as number;
  if ('furiganaMode' in values) {
    // furiganaMode arrives as the discrete option's numeric index; map it
    // back to the stable id string for the subtitle store.
    const idx = values.furiganaMode as number;
    current.furiganaMode = FURIGANA_IDS[idx] ?? 'all';
  }
  localStorage.setItem('subtitleSettings', JSON.stringify(current));
}

/**
 * Build the client-only "Interface" category: language + per-slot fonts.
 * These sit above the C++-provided categories in the tab bar.
 */
function buildInterfaceCategory(currentLocale: Locale): SettingsCategory {
  const localeIdx = Math.max(0, LOCALE_IDS.indexOf(currentLocale));
  const localeLabels = LOCALE_IDS.map(id => LOCALE_LABELS[id]);

  // Per-slot font choices — translate option labels into the current locale
  // so the dropdown list reads naturally on first open. A locale change
  // later will re-run loadSchema if OpenSettings is called again; for live
  // re-translation of dropdown items we'd need to refresh on locale change.
  const buildFontSetting = (slot: FontSlot, labelKey: LocaleKeys, descKey: LocaleKeys): SettingDef => {
    const slotCfg = FONT_SLOTS[slot];
    const currentId = getSavedFontId(slot);
    const idx = Math.max(0, slotCfg.allowedIds.indexOf(currentId));
    const options = slotCfg.allowedIds.map(id => getFontLabel(id, currentLocale));
    return {
      type: 'discrete',
      value: idx,
      label: labelKey,               // labelKey takes over at render time
      labelKey,
      descriptionKey: descKey,
      description: descKey,
      options,
      ids: slotCfg.allowedIds,
    };
  };

  return {
    label: 'settings.category.interface',
    labelKey: 'settings.category.interface',
    settings: {
      language: {
        type: 'discrete',
        value: localeIdx,
        label: 'settings.label.language',
        labelKey: 'settings.label.language',
        description: 'settings.desc.language',
        descriptionKey: 'settings.desc.language',
        options: localeLabels,
        ids: LOCALE_IDS,
      },
      uiFont: buildFontSetting('ui', 'settings.label.ui_font', 'settings.desc.ui_font'),
      displayFont: buildFontSetting('display', 'settings.label.display_font', 'settings.desc.display_font'),
      subtitleFont: buildFontSetting('subtitle', 'settings.label.subtitle_font', 'settings.desc.subtitle_font'),
    },
  };
}

// ===== Store =====

export const useSettingsStore = create<SettingsState>((set, get) => ({
  schema: null,
  original: {},
  values: {},
  isDirty: false,
  uiScale: parseFloat(localStorage.getItem('uiScale') ?? '2'),

  loadSchema: (schema) => {
    const currentLocale = useI18nStore.getState().locale;

    // ── Inject client-only "Interface" category at the top ──
    const interfaceCat = buildInterfaceCategory(currentLocale);
    schema = { interface: interfaceCat, ...schema };

    // ── Tag known C++-provided categories with i18n keys ──
    const CATEGORY_KEYS: Record<string, LocaleKeys> = {
      audio:         'settings.category.audio',
      video:         'settings.category.video',
      input:         'settings.category.input',
      accessibility: 'settings.category.accessibility',
    };
    for (const [key, cat] of Object.entries(schema)) {
      const labelKey = CATEGORY_KEYS[key];
      if (labelKey) cat.labelKey = labelKey;
    }

    // ── Tag known C++-provided settings with i18n keys ──
    // C++ sends English labels by default; here we attach labelKey/optionsKey
    // so Settings.tsx can re-localize live on language change without a
    // schema reload. Only well-known keys get the treatment; unknown C++
    // keys keep their English fallback.
    if (schema.input?.settings?.sprintMode) {
      const def = schema.input.settings.sprintMode;
      def.labelKey = 'settings.label.sprint_mode';
      def.descriptionKey = 'settings.desc.sprint_mode';
      def.optionsKey = [
        'settings.option.sprint_mode.hold',
        'settings.option.sprint_mode.toggle',
      ];
      def.optionDescriptions = def.optionDescriptions ?? [
        'settings.option.sprint_mode.hold.desc',
        'settings.option.sprint_mode.toggle.desc',
      ];
      def.optionDescriptionsKey = [
        'settings.option.sprint_mode.hold.desc',
        'settings.option.sprint_mode.toggle.desc',
      ];
    }

    // ── Inject client-side subtitle settings into accessibility ──
    if (schema.accessibility) {
      const subtitleSaved = loadSubtitleSettings();
      const subtitleSettings: Record<string, SettingDef> = {
        _subtitleHeader: {
          type: 'subtitle',
          label: 'settings.subtitle.header',
          labelKey: 'settings.subtitle.header',
          value: 0,
        },
        showSpeakerName: {
          type: 'toggle', value: subtitleSaved.showSpeakerName,
          label: 'settings.label.show_speaker_name',
          labelKey: 'settings.label.show_speaker_name',
          description: 'settings.desc.show_speaker_name',
          descriptionKey: 'settings.desc.show_speaker_name',
        },
        showSpeakerColor: {
          type: 'toggle', value: subtitleSaved.showSpeakerColor,
          label: 'settings.label.show_speaker_color',
          labelKey: 'settings.label.show_speaker_color',
          description: 'settings.desc.show_speaker_color',
          descriptionKey: 'settings.desc.show_speaker_color',
          visibleWhen: { key: 'showSpeakerName', equals: true },
        },
        subtitleFontScale: {
          type: 'scalar', value: subtitleSaved.fontScale,
          min: 0.5, max: 2.0, step: 0.1,
          label: 'settings.label.subtitle_font_scale',
          labelKey: 'settings.label.subtitle_font_scale',
          description: 'settings.desc.subtitle_font_scale',
          descriptionKey: 'settings.desc.subtitle_font_scale',
        },
        subtitleBgOpacity: {
          type: 'scalar', value: subtitleSaved.bgOpacity,
          min: 0, max: 1, step: 0.1,
          label: 'settings.label.subtitle_bg_opacity',
          labelKey: 'settings.label.subtitle_bg_opacity',
          description: 'settings.desc.subtitle_bg_opacity',
          descriptionKey: 'settings.desc.subtitle_bg_opacity',
        },
        // Japanese-only: ruby (furigana) density. Hidden via `visibleWhen`
        // unless the language slot's current value points at the 'ja' index,
        // so non-Japanese players never see this setting.
        //
        // optionsKey + optionDescriptionsKey go through Settings.tsx live
        // translation so the labels and per-option explanations re-localize
        // when the player switches language without a schema reload.
        furiganaMode: {
          type: 'discrete',
          value: Math.max(0, FURIGANA_IDS.indexOf(subtitleSaved.furiganaMode)),
          options: [
            'settings.option.furigana.off',
            'settings.option.furigana.key',
            'settings.option.furigana.all',
          ],
          optionsKey: [
            'settings.option.furigana.off',
            'settings.option.furigana.key',
            'settings.option.furigana.all',
          ],
          optionDescriptions: [
            'settings.option.furigana.off.desc',
            'settings.option.furigana.key.desc',
            'settings.option.furigana.all.desc',
          ],
          optionDescriptionsKey: [
            'settings.option.furigana.off.desc',
            'settings.option.furigana.key.desc',
            'settings.option.furigana.all.desc',
          ],
          ids: [...FURIGANA_IDS],
          label: 'settings.label.furigana_mode',
          labelKey: 'settings.label.furigana_mode',
          description: 'settings.desc.furigana_mode',
          descriptionKey: 'settings.desc.furigana_mode',
          visibleWhen: { key: 'language', equals: LOCALE_IDS.indexOf('ja') },
        },
      };
      // ── Inject client-side "市场" (market) subsection into accessibility ──
      // 涨跌配色是显示/无障碍偏好（含红绿色盲友好方案），纯前端，不进 C++。
      const marketColorIdx = Math.max(0, MARKET_COLOR_IDS.indexOf(loadMarketColorMode()));
      const marketSettings: Record<string, SettingDef> = {
        _marketHeader: {
          type: 'subtitle',
          label: 'settings.market.header',
          labelKey: 'settings.market.header',
          value: 0,
        },
        marketColorMode: {
          type: 'discrete',
          value: marketColorIdx,
          options: [
            'settings.option.market_color.red_up',
            'settings.option.market_color.green_up',
            'settings.option.market_color.colorblind',
          ],
          optionsKey: [
            'settings.option.market_color.red_up',
            'settings.option.market_color.green_up',
            'settings.option.market_color.colorblind',
          ],
          optionDescriptions: [
            'settings.option.market_color.red_up.desc',
            'settings.option.market_color.green_up.desc',
            'settings.option.market_color.colorblind.desc',
          ],
          optionDescriptionsKey: [
            'settings.option.market_color.red_up.desc',
            'settings.option.market_color.green_up.desc',
            'settings.option.market_color.colorblind.desc',
          ],
          ids: [...MARKET_COLOR_IDS],
          label: 'settings.label.market_color',
          labelKey: 'settings.label.market_color',
          description: 'settings.desc.market_color',
          descriptionKey: 'settings.desc.market_color',
        },
      };

      const existingSettings = schema.accessibility.settings;
      schema.accessibility.settings = { ...subtitleSettings, ...existingSettings, ...marketSettings };

      // Also localize the ui_scale entry if present
      if (schema.accessibility.settings.uiScale) {
        schema.accessibility.settings.uiScale.labelKey = 'settings.label.ui_scale';
        schema.accessibility.settings.uiScale.descriptionKey = 'settings.desc.ui_scale';
      }

      // Tag the color-blind mode setting so the detail panel renders the
      // simulated-palette preview. We match by key OR by english label so
      // C++ can name it either 'colorBlindMode' or 'Color Blind Mode'.
      for (const [k, def] of Object.entries(schema.accessibility.settings)) {
        const lab = (def.label || '').toLowerCase();
        if (k === 'colorBlindMode' || lab.includes('color blind') || lab.includes('colorblind')) {
          def.previewKind = 'colorblind';
        }
      }
    }

    const vals = extractValues(schema);
    // uiScale is client-side only — use persisted localStorage value, not C++ default
    const savedScale = get().uiScale;
    vals.uiScale = savedScale;
    if (schema.accessibility?.settings?.uiScale) {
      schema.accessibility.settings.uiScale.value = savedScale;
    }
    set({
      schema,
      original: { ...vals },
      values: { ...vals },
      isDirty: false,
      uiScale: savedScale,
    });
  },

  setValue: (key, value) => {
    const { values, original } = get();
    const next = { ...values, [key]: value };

    let dirty = false;
    for (const k of Object.keys(next)) {
      if (next[k] !== original[k]) {
        dirty = true;
        break;
      }
    }

    const update: Partial<SettingsState> = { values: next, isDirty: dirty };

    // ── Live preview for client-only UI settings ──
    if (key === 'uiScale' && typeof value === 'number') {
      update.uiScale = value;
      localStorage.setItem('uiScale', String(value));
    }
    else if (key === 'language' && typeof value === 'number') {
      const locale = LOCALE_IDS[value];
      if (locale) useI18nStore.getState().setLocale(locale);
    }
    else if ((key === 'uiFont' || key === 'displayFont' || key === 'subtitleFont') && typeof value === 'number') {
      const slot: FontSlot = key === 'uiFont' ? 'ui' : key === 'displayFont' ? 'display' : 'subtitle';
      const slotCfg = FONT_SLOTS[slot];
      const fontId = slotCfg.allowedIds[value];
      if (fontId && FONTS[fontId]) applyFont(slot, fontId);
    }
    else if (key === 'furiganaMode' && typeof value === 'number') {
      // Live-preview the furigana density on the running subtitle so the
      // player can see the difference between modes without clicking Apply.
      // (Other subtitle settings keep their existing apply-on-Apply path —
      // adding live preview to all of them is a separate refactor.)
      useSubtitleStore.getState().updateSettings({
        furiganaMode: FURIGANA_IDS[value] ?? 'all',
      });
    }

    set(update);
  },

  apply: () => {
    const { values, original } = get();
    // Only send changed values
    const changes: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(values)) {
      if (v !== original[k]) {
        changes[k] = v;
      }
    }
    if (Object.keys(changes).length > 0) {
      // Client-side persistence for injected settings
      syncSubtitleSettings(changes);
      if ('marketColorMode' in changes) {
        const idx = changes.marketColorMode as number;
        saveMarketColorMode(MARKET_COLOR_IDS[idx] ?? 'redUp');
      }
      // Language / fonts already persist on setValue (live-preview path),
      // so nothing extra to do here — just keep them out of the UE payload.

      // Filter out client-only keys before sending to C++
      const ueChanges: Record<string, unknown> = {};
      for (const [k, v] of Object.entries(changes)) {
        if (!CLIENT_ONLY_KEYS.has(k)) ueChanges[k] = v;
      }
      if (Object.keys(ueChanges).length > 0) {
        sendToUE('settings_apply', ueChanges);
      }
    }
    // Notify subtitle store to reload settings from localStorage
    useSubtitleStore.getState().reloadSettings();

    // Update original to current (settings are now saved)
    set({ original: { ...values }, isDirty: false });
  },

  cancel: () => {
    const { values, original } = get();
    // Revert live-previewed client settings back to their "original" state
    // so "Cancel" actually cancels the preview, not just the dirty marker.
    if (values.language !== original.language) {
      const locale = LOCALE_IDS[original.language as number];
      if (locale) useI18nStore.getState().setLocale(locale);
    }
    if (values.uiFont !== original.uiFont) {
      const id = FONT_SLOTS.ui.allowedIds[original.uiFont as number];
      if (id) applyFont('ui', id);
    }
    if (values.displayFont !== original.displayFont) {
      const id = FONT_SLOTS.display.allowedIds[original.displayFont as number];
      if (id) applyFont('display', id);
    }
    if (values.subtitleFont !== original.subtitleFont) {
      const id = FONT_SLOTS.subtitle.allowedIds[original.subtitleFont as number];
      if (id) applyFont('subtitle', id);
    }
    if (values.furiganaMode !== original.furiganaMode) {
      const idx = original.furiganaMode as number;
      useSubtitleStore.getState().updateSettings({
        furiganaMode: FURIGANA_IDS[idx] ?? 'all',
      });
    }
    if (values.uiScale !== original.uiScale) {
      const s = original.uiScale as number;
      localStorage.setItem('uiScale', String(s));
      set({ uiScale: s });
    }

    set({ values: { ...original }, isDirty: false });
  },
}));
