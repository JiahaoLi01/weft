/**
 * Lightweight, type-safe i18n for the CRPG WebUI.
 *
 * Design decisions:
 *  - zh-CN is the **source-of-truth** locale; its keys define the `LocaleKeys` type.
 *  - Every other locale file is typed as `Record<LocaleKeys, string>`, so a
 *    missing or extra key is a compile-time error.
 *  - A Zustand store (`i18n-store.ts`) holds the active locale and exposes a
 *    `t()` function that components use via the `useT()` hook.
 *  - No runtime key interpolation for now — keep it dead simple.  If we later
 *    need `{count}` style placeholders we can add a thin formatter.
 */

import zhCN, { type LocaleKeys } from './locales/zh-CN';
import en from './locales/en';
import ja from './locales/ja';

// ── Supported locales ────────────────────────────────────────

export type Locale = 'zh-CN' | 'en' | 'ja';

export const LOCALE_LABELS: Record<Locale, string> = {
  'zh-CN': '简体中文',
  'en':    'English',
  'ja':    '日本語',
};

const localeMaps: Record<Locale, Record<LocaleKeys, string>> = {
  'zh-CN': zhCN,
  'en':    en,
  'ja':    ja,
};

// ── Translate function factory ───────────────────────────────

/**
 * Returns the translated string for `key` in the given `locale`.
 * Falls back to zh-CN if a key is somehow missing at runtime.
 */
export function translate(locale: Locale, key: LocaleKeys): string {
  return localeMaps[locale]?.[key] ?? zhCN[key] ?? key;
}

// Re-export types so consumers only import from `@/i18n`
export type { LocaleKeys };
