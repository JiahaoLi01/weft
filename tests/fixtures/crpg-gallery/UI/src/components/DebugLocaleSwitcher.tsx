import { useI18nStore } from '@/stores/i18n-store';
import { LOCALE_LABELS, type Locale } from '@/i18n';

const LOCALES = Object.entries(LOCALE_LABELS) as [Locale, string][];

/**
 * Tiny bottom-right badge to cycle locales.
 * Only rendered in dev mode (outside UE) for debugging.
 *
 * We check for the native `ue5()` function which only exists in CEF,
 * not the polyfilled `ue.interface` that our bridge creates in dev.
 */
export function DebugLocaleSwitcher() {
  const { locale, setLocale } = useI18nStore();

  // Hide inside real UE CEF — `ue5` is injected natively by CEF, not polyfilled
  if (typeof window.ue5 === 'function') return null;

  const cycle = () => {
    const idx = LOCALES.findIndex(([l]) => l === locale);
    const next = LOCALES[(idx + 1) % LOCALES.length][0];
    setLocale(next);
  };

  return (
    <button
      onClick={cycle}
      className="fixed bottom-3 right-3 z-50 px-3 py-1.5 rounded bg-panel/90 border border-panel-border
                 text-xs font-mono text-gold/80 hover:text-gold hover:bg-panel-light
                 backdrop-blur-sm transition-all duration-200 cursor-pointer select-none"
      title={`Current: ${locale}\nClick to switch`}
    >
      {LOCALE_LABELS[locale]}
    </button>
  );
}
