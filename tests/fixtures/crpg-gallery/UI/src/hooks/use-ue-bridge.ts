import { useEffect } from 'react';
import { onUECall, sendToUE, isInUnreal, initWebSocket, destroyWebSocket } from '@/lib/ue-bridge';
import { useGameStore } from '@/stores/game-store';
import { useSettingsStore, type SettingsSchema } from '@/stores/settings-store';
import { useI18nStore } from '@/stores/i18n-store';
import type { Locale } from '@/i18n';

/**
 * Global UE bridge hook — call once at App root.
 *
 * Registers global handlers that are always active.
 * UI stack commands (pushUI/popUI/replaceUI/clearUI) are registered
 * directly in ui-store.ts at module load time — no need to repeat here.
 */
export function useUEBridge(): void {
  useEffect(() => {
    const cleanups: (() => void)[] = [];

    initWebSocket();

    // Legacy navigate support — maps to replaceUI for backward compat
    cleanups.push(
      onUECall('navigate', async (data: unknown) => {
        const { useUIStore } = await import('@/stores/ui-store');
        const page = (typeof data === 'string' ? data : (data as { page: string })?.page);
        if (page) useUIStore.getState().replace(page);
      }),
    );

    // Settings — C++ pushes schema+values when OpenSettings() is called
    cleanups.push(
      onUECall('loadSettings', (data: unknown) => {
        useSettingsStore.getState().loadSchema(data as SettingsSchema);
      }),
    );

    // Locale — UE can push locale changes
    cleanups.push(
      onUECall('setLocale', (data: unknown) => {
        const locale = (typeof data === 'string' ? data : (data as { locale: string })?.locale) as Locale;
        if (locale) {
          useI18nStore.getState().setLocale(locale);
        }
      }),
    );

    // Mark connected and notify UE that handlers are registered
    useGameStore.getState().setConnected(true);
    sendToUE('ui_ready');

    if (!isInUnreal()) {
      console.log('[UE Bridge] Running in dev mode (outside UE)');
    }

    return () => {
      cleanups.forEach(fn => fn());
      destroyWebSocket();
    };
  }, []);
}
