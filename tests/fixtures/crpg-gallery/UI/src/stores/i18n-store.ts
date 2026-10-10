import { useCallback } from 'react';
import { create } from 'zustand';
import { type Locale, type LocaleKeys, translate } from '@/i18n';

const LOCALE_STORAGE_KEY = 'locale';
const SUPPORTED_LOCALES: readonly Locale[] = ['zh-CN', 'en', 'ja'];

/** Load the persisted locale, defaulting to zh-CN if absent or invalid. */
function loadInitialLocale(): Locale {
  try {
    const saved = localStorage.getItem(LOCALE_STORAGE_KEY) as Locale | null;
    if (saved && SUPPORTED_LOCALES.includes(saved)) return saved;
  } catch { /* ignore */ }
  return 'zh-CN';
}

export interface I18nState {
  locale: Locale;

  /** Switch the active locale. Persists to localStorage. */
  setLocale: (locale: Locale) => void;
}

export const useI18nStore = create<I18nState>((set) => ({
  locale: loadInitialLocale(),

  setLocale: (locale) => {
    try { localStorage.setItem(LOCALE_STORAGE_KEY, locale); } catch { /* ignore */ }
    set({ locale });
  },
}));

/**
 * Returns a `t()` function that re-renders when locale changes.
 *
 * Usage:
 *   const t = useT();
 *   <span>{t('menu.start_game')}</span>
 */
export function useT() {
  const locale = useI18nStore((s) => s.locale);
  return useCallback((key: LocaleKeys) => translate(locale, key), [locale]);
}
