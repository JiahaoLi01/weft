/**
 * Battle palette —— 战斗 HP / 状态读数共用的"古董雕版"配色。
 *
 * 面板内 HpBar(battle/HpBar.tsx)和浮空血条(floating-ui/HealthBar.tsx)从这里取
 * 同一套三档油画色阶 + 黄铜框架色,保证两处 HP 读数读作同一件仪器;buff chip /
 * cost chip / 属性增减值的三态 tone 也从这里取。维多利亚新古典约束:油画色、
 * 低饱和、平面、无霓虹无光晕。
 *
 * ANTIQUE_GOLD 与 theme.css 的 `--color-brass` 是同一个值 —— CSS 类走 text-brass /
 * border-brass,JS 端字符串拼接走这里。两边改动要同步。
 */

export const ANTIQUE_GOLD = '#c7a667';
export const BAR_GROUND = '#15100a';
export const NAME_INK = '#e9ddc1';

/** rgba 黄铜 —— 细线框 / 分隔线在不同透明度下复用同一色相。 */
export function brass(alpha: number): string {
  return `rgba(199, 166, 103, ${alpha})`;
}

/** rgba 石榴红(oxblood)—— 负面 / 濒危。 */
export function garnet(alpha: number): string {
  return `rgba(158, 63, 55, ${alpha})`;
}

/** rgba 月桂绿(verdigris)—— 正面 / 健康。 */
export function laurel(alpha: number): string {
  return `rgba(94, 143, 106, ${alpha})`;
}

/**
 * rgba 琥珀(amber)—— **警告 / debuff**。刻意区别于 garnet:garnet 红 = "禁止 / 不可用"
 * (付不起、不可达、被拒),amber 琥珀 = "可用但更差"(被改贵的消耗、减益)。橙相,介于
 * 古金与石榴红之间,三者色相互不混淆。配套 ink 见各处内联(约 #dba05c)。
 */
export function amber(alpha: number): string {
  return `rgba(192, 120, 52, ${alpha})`;
}

export interface HpTierPalette {
  /** 填充渐变(竖向三段,油画质感) */
  fill: string;
  /** 顶部 1px catch-light(手绘高光线)颜色 */
  lit: string;
  /** 同档位的文本 / 图标 ink 色(深底上可读的浅档) */
  ink: string;
}

/**
 * HP 百分比 → 三档油画色阶。≥60% 月桂绿,≥30% 古金赭,<30% 石榴红。
 * 档位切换是色相突变 —— 信息密度最高,玩家一眼读出档位。
 */
export function hpTierPalette(pct: number): HpTierPalette {
  if (pct >= 0.6) {
    // Laurel / verdigris green
    return {
      fill: 'linear-gradient(180deg, #8bb392 0%, #5e8f6a 46%, #3f6b50 100%)',
      lit: 'rgba(216, 238, 216, 0.5)',
      ink: '#9ec9a4',
    };
  }
  if (pct >= 0.3) {
    // Antique ochre / old gold
    return {
      fill: 'linear-gradient(180deg, #ddc079 0%, #c39a46 46%, #936e26 100%)',
      lit: 'rgba(247, 233, 190, 0.5)',
      ink: '#d8bd7e',
    };
  }
  // Garnet / oxblood
  return {
    fill: 'linear-gradient(180deg, #c0726a 0%, #9e3f37 46%, #6a241f 100%)',
    lit: 'rgba(240, 202, 198, 0.46)',
    ink: '#cf8e86',
  };
}

/**
 * Buff / 修改器三态 tone ink —— chips、属性增减值等小元素的统一文本着色。
 * 比 Tailwind emerald-300 / rose-300 低饱和一档,油画而非霓虹。
 */
export const TONE_INK = {
  positive: '#a3c4a3',
  negative: '#cf8e86',
  neutral: ANTIQUE_GOLD,
} as const;
