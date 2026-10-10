/**
 * 战斗面板共享装饰组件 —— BattleUnitPanel / BattleUnitStatusPanel 共用,
 * 让两个面板视觉上同语言(同款角饰 / 同款分隔线 / 同款小节标题)。
 *
 * 不放 lucide-react 图标 —— 这些装饰是项目特定的"D&D 古书"风格,跟泛用 icon 库
 * 走的"现代 line-icon"语言不一致。每条都是 inline SVG,SVG 路径用 currentColor
 * 着色,可被父级 text-* 颜色驱动(默认走 panel-border 那条金色)。
 *
 * 性能:这些都是无 state 的纯渲染组件,SVG 内联在 JSX 里,无外部资源加载。
 * 浮点路径数据走 path d 直接写死,UE / React 双端渲染都稳定。
 */

import type { CSSProperties, ReactNode } from 'react';

// ─── Panel Surface ───────────────────────────────────────────────
//
// 战斗面板共用的"卡面"样式 —— 维多利亚证书式:外层 1px 黄铜细线框 + 内层
// PanelInnerRule 回声细线(双线规),平面墨色渐变底,无内嵌斜面、无光晕。
// BattleUnitPanel / BattleUnitStatusPanel 直接 spread 这份 style,保证两个
// panel 永远同一张"纸"。半透明底(0.86–0.92 alpha,世界隐约透出)是验证过的
// 想要的质感;外投影 14px 模糊半径也是之前调过的性能/质感平衡点,都不动。

export const BATTLE_PANEL_SURFACE: CSSProperties = {
  background: 'linear-gradient(160deg, rgba(28,20,12,0.92) 0%, rgba(20,15,10,0.86) 60%, rgba(28,20,12,0.92) 100%)',
  border: '1px solid rgba(199,166,103,0.45)',
  borderRadius: 4,
  boxShadow: '0 4px 14px rgba(0,0,0,0.55)',
};

/**
 * 面板内圈回声细线 —— 跟外框 border 构成双线规(double rule),维多利亚印刷
 * 证书 / 藏书票边框的标志性母题。0 blur 的 1px border,渲染开销可忽略。
 * 挂在 `position: relative` 的 panel 里,放在内容之前(被内容覆盖也无妨,纯装饰)。
 */
export function PanelInnerRule() {
  return (
    <div
      aria-hidden
      className="pointer-events-none absolute"
      style={{ inset: 3, border: '1px solid rgba(199,166,103,0.18)', borderRadius: 2 }}
    />
  );
}

// ─── Corner Ornaments ────────────────────────────────────────────
//
// 四角装饰 —— 双线规角 + 角点菱形,呼应面板外框的双层细线(雕版证书角花的
// 简化形)。用 absolute 定位到父级 panel 的四个角(父级需要 relative 或 absolute)。
//
// 单个 SVG 设计成左上角朝向,其它三个角用 CSS transform 旋转复用,
// 同一份 path 数据走四份不同方向。size prop 控制角饰边长(默认 16px)。

interface CornerSpriteProps {
  size: number;
  rotate: number;  // 0 / 90 / 180 / 270 — 顺时针,匹配 left-top / right-top / right-bottom / left-bottom
  className?: string;
}

function CornerSprite({ size, rotate, className = '' }: CornerSpriteProps) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 16 16"
      fill="none"
      style={{ transform: `rotate(${rotate}deg)` }}
      className={className}
      // pointer-events-none —— 装饰不该截 click / hover 给底下的 panel 内容
    >
      {/* 外侧主 L 线 */}
      <path d="M 1 10 L 1 1 L 10 1" stroke="currentColor" strokeWidth="1.1" strokeLinecap="square" />
      {/* 线端 serif 终结符 —— 雕版线脚,短垂直小横线收尾 */}
      <path d="M 0.2 10 L 1.8 10 M 10 0.2 L 10 1.8" stroke="currentColor" strokeWidth="0.9" strokeLinecap="round" opacity="0.9" />
      {/* 内侧回声 L —— 双线规,呼应面板的双层边框 */}
      <path d="M 4.2 8.5 L 4.2 4.2 L 8.5 4.2" stroke="currentColor" strokeWidth="0.7" strokeLinecap="round" opacity="0.55" />
      {/* 角点菱形 —— 嵌在双线之间的小宝石位 */}
      <path d="M 2.6 1.2 L 4 2.6 L 2.6 4 L 1.2 2.6 Z" fill="currentColor" opacity="0.9" />
    </svg>
  );
}

interface CornerOrnamentsProps {
  /** 角饰尺寸(px),默认 14 */
  size?: number;
  /** Tailwind 颜色类(driving currentColor),默认 text-gold/60 */
  colorClass?: string;
  /** 角饰跟 panel 边缘的距离(px),默认 4 */
  offset?: number;
}

/**
 * 四角装饰花纹。挂在父级 `position: relative` 的 panel 内,absolute 定位到四角。
 * 设计上是"装饰",pointer-events-none 让 click / hover 透传到下面的内容。
 */
export function CornerOrnaments({
  size = 14,
  colorClass = 'text-gold/60',
  offset = 4,
}: CornerOrnamentsProps) {
  // inset 用 inline style 不进 Tailwind —— offset 是 prop,不能用 Tailwind 静态类
  return (
    <div className={`pointer-events-none absolute inset-0 ${colorClass}`} aria-hidden>
      <div className="absolute" style={{ top: offset, left: offset }}>
        <CornerSprite size={size} rotate={0} />
      </div>
      <div className="absolute" style={{ top: offset, right: offset }}>
        <CornerSprite size={size} rotate={90} />
      </div>
      <div className="absolute" style={{ bottom: offset, right: offset }}>
        <CornerSprite size={size} rotate={180} />
      </div>
      <div className="absolute" style={{ bottom: offset, left: offset }}>
        <CornerSprite size={size} rotate={270} />
      </div>
    </div>
  );
}

// ─── Ornate Divider ──────────────────────────────────────────────
//
// 横向分隔线,中心带一个菱形(diamond)花纹。用作面板内 section 之间的分隔,比纯
// border-b 有"古书装订线"质感。可选 children 在中心(替代 diamond,例如放小节标题)。

interface OrnateDividerProps {
  /** 替换中心 diamond 的内容(放小节标题时用)。无内容时默认渲染 diamond。 */
  children?: ReactNode;
  /** 线条颜色(默认 panel-border) */
  className?: string;
}

export function OrnateDivider({ children, className = '' }: OrnateDividerProps) {
  return (
    <div className={`flex items-center gap-2 ${className}`} aria-hidden>
      {/* 左侧渐变线:实线 → 半透明,做"线条向中心聚焦"的效果 */}
      <div className="h-px flex-1 bg-gradient-to-r from-transparent via-brass/35 to-brass/35" />
      {children ?? (
        // 默认中心花饰:菱形 + 两侧小圆点(· ◆ ·)—— 维多利亚书名页分隔线的标准母题
        <svg width="22" height="8" viewBox="0 0 22 8" fill="none" className="text-brass/70 shrink-0">
          <circle cx="3" cy="4" r="0.9" fill="currentColor" opacity="0.6" />
          <path d="M 11 0.8 L 14.2 4 L 11 7.2 L 7.8 4 Z" stroke="currentColor" strokeWidth="0.8" fill="currentColor" fillOpacity="0.25" />
          <circle cx="19" cy="4" r="0.9" fill="currentColor" opacity="0.6" />
        </svg>
      )}
      <div className="h-px flex-1 bg-gradient-to-l from-transparent via-brass/35 to-brass/35" />
    </div>
  );
}

// ─── Section Header ───────────────────────────────────────────────
//
// 小节标题:左侧可选 icon + 中间小写字标签 + 右侧可选 trailing 内容(例:数量徽章)。
// 走 subtitle font(Cinzel Decorative)+ tracking-widest + uppercase,显著区别于 body。

interface SectionHeaderProps {
  /** 标签文字(显示成 uppercase 小字) */
  label: string;
  /** 左侧 icon —— 通常传 lucide 图标 */
  icon?: ReactNode;
  /** 右侧 trailing 内容,例如计数 badge / 折叠按钮 */
  trailing?: ReactNode;
}

export function SectionHeader({ label, icon, trailing }: SectionHeaderProps) {
  return (
    <div className="flex items-center gap-1.5 text-[10px] tracking-widest uppercase text-parchment-dark/80">
      {icon && <span className="text-brass/80 [&_svg]:size-3">{icon}</span>}
      <span className="font-[family-name:var(--font-subtitle)] leading-none">{label}</span>
      {trailing && <span className="ml-auto">{trailing}</span>}
    </div>
  );
}
