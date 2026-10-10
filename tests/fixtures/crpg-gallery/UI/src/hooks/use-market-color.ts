import { useSettingsStore } from '@/stores/settings-store';
import {
  MARKET_COLOR_IDS,
  loadMarketColorMode,
  paletteFor,
  type MarketColorMode,
  type MarketPalette,
} from '@/config/market-colors';

/**
 * 当前市场涨跌配色模式。优先用设置页里正在编辑/已应用的值（实时预览 + 应用后即时反映），
 * 设置从未打开过时回退到 localStorage 持久值。settings-store 里 marketColorMode 存的是 discrete
 * 选项的数字下标。
 */
export function useMarketColorMode(): MarketColorMode {
  return useSettingsStore((s) => {
    const idx = s.values['marketColorMode'];
    if (typeof idx === 'number' && MARKET_COLOR_IDS[idx]) {
      return MARKET_COLOR_IDS[idx];
    }
    return loadMarketColorMode();
  });
}

/** 当前配色对应的涨/跌颜色（tailwind 类 + SVG hex）。 */
export function useMarketPalette(): MarketPalette {
  return paletteFor(useMarketColorMode());
}
