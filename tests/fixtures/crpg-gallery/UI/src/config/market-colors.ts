/**
 * 市场涨跌配色方案（玩家偏好，无障碍）。纯数据 + localStorage 读写，**不依赖任何 store**
 * （settings-store 反过来 import 本文件，放在这里避免循环依赖）。
 *
 * 中国市场惯例红涨绿跌；欧美绿涨红跌；外加一套红绿色盲友好的橙/蓝。可继续扩展。
 */
export type MarketColorMode = 'redUp' | 'greenUp' | 'colorblind';

/** index ↔ id：对应 settings 里 discrete 选项的顺序。 */
export const MARKET_COLOR_IDS: MarketColorMode[] = ['redUp', 'greenUp', 'colorblind'];

export interface MarketPalette {
  /** 涨：tailwind 文字类（页面文本用）。 */
  upClass: string;
  /** 跌：tailwind 文字类。 */
  downClass: string;
  /** 涨：SVG 填充色（蜡烛用）。 */
  upHex: string;
  /** 跌：SVG 填充色。 */
  downHex: string;
}

const PALETTES: Record<MarketColorMode, MarketPalette> = {
  redUp: { upClass: 'text-rose-400', downClass: 'text-emerald-400', upHex: '#c2543f', downHex: '#4e9c6d' },
  greenUp: { upClass: 'text-emerald-400', downClass: 'text-rose-400', upHex: '#4e9c6d', downHex: '#c2543f' },
  colorblind: { upClass: 'text-amber-400', downClass: 'text-sky-400', upHex: '#d9a441', downHex: '#5b8fd0' },
};

export function paletteFor(mode: MarketColorMode): MarketPalette {
  return PALETTES[mode] ?? PALETTES.redUp;
}

const LS_KEY = 'marketColorMode';

export function loadMarketColorMode(): MarketColorMode {
  try {
    const saved = localStorage.getItem(LS_KEY);
    if (saved && (MARKET_COLOR_IDS as string[]).includes(saved)) {
      return saved as MarketColorMode;
    }
  } catch {
    /* ignore */
  }
  return 'redUp';
}

export function saveMarketColorMode(mode: MarketColorMode): void {
  try {
    localStorage.setItem(LS_KEY, mode);
  } catch {
    /* ignore */
  }
}
