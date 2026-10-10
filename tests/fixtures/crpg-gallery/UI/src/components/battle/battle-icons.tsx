/**
 * 战斗 UI icon 映射 —— attribute / resource tag → icon 组件。
 *
 * **资源图标(2026-07-13 起)是正式设计**,与技能图标(ability-icons.tsx)同一套
 * 版画墨章语言,但按渲染尺寸做了减法:资源图标出现在资源铭牌(14px)和技能消耗
 * chip(12px)上,所以 —— 粗实剪影、**至多一处负空间镂刻、没有淡墨二印层**
 * (12px 下细节会糊成噪点,负空间镂孔在最小尺寸下优雅地闭合、不影响轮廓辨识)。
 *
 * 形状语义系统(试玩反馈"图标太随意认不出",这版每类资源一个专属轮廓):
 *   - 主动作     = 实心圆章(一枚完整的行动印记;BG3 玩家肌肉记忆:圆 = 动作)
 *   - 附赠动作   = 实心三角(BG3 肌肉记忆:三角 = 附赠动作)
 *   - 反应       = 盾形(被动/防御窗口)
 *   - 移动力     = 双箭头 »(前进)
 *   - 子弹       = 弹壳侧影(字面义)
 *   - 法术环     = 圆环("环"的字面义;一至四环 = 环下 1-4 颗计数点)
 *   - 虚法术位   = 断环(会消逝的环)
 *   - 基础资源伞 = 四点簇(一捆基础资源)
 * 圆章(实心为主) vs 法术环(空心为主)刻意拉开实/空对比,最小尺寸也不混淆。
 *
 * Attribute 侧仍是 lucide 占位(属性面板尺寸大、数量多,等美术批量方案)。
 *
 * Tag tree partial-match:resource tag 用最长前缀匹配,`Resource.SpellSlot.L3`
 * 没单独配映射时 fallback 到 `Resource.SpellSlot`。跟 C++ tag tree 语义对齐。
 */

import type { FC, ReactNode } from 'react';
import {
  // Attributes
  Heart,
  Droplet,
  Swords,
  Shield,
  Sparkles,
  Zap,
  Footprints,
  // Resources(旧占位,仍 re-export 给别处用)
  Hourglass,
  Flame,
  Snowflake,
  Crown,
  // Generic
  Hexagon,
  Star,
  type LucideIcon,
} from 'lucide-react';

// ─── Attribute icon map ──────────────────────────────────────────
//
// Key = attribute id 全名(`ClassName.PropertyName`,跟 C++ FGameplayAttributeField 一致)。
// 后续加 Strength / Dexterity / AC 等只需要在这里补条目。
// 混合期:HP/MP 仍是 lucide 占位,新属性逐个换版画剪影(条目类型已放宽兼容两者)。

/** lucide 组件与版画剪影的公共可渲染类型(call site 都传 className + strokeWidth)。 */
export type BattleIconComponent = FC<{ className?: string; strokeWidth?: number }>;

const ATTRIBUTE_ICONS: Record<string, BattleIconComponent> = {
  'CRPGGameplayAttributeData.MaxHealth':        Heart,
  'CRPGGameplayAttributeData.MaxMana':          Droplet,
  'CRPGGameplayAttributeData.ArmorThickness':   ArmorThicknessGlyph,
  'CRPGGameplayAttributeData.NightVisionRange': NightVisionGlyph,
};

/** 拿 attribute icon,未配置时返 fallback(Hexagon —— 通用占位)。 */
export function getAttributeIcon(attributeId: string): BattleIconComponent {
  return ATTRIBUTE_ICONS[attributeId] ?? Hexagon;
}

// ─── Resource glyphs(版画剪影)─────────────────────────────────
//
// 跟 lucide 组件同形调用:call site 传 className + strokeWidth。strokeWidth 对实心
// 剪影无意义,收下并忽略 —— 保持 createElement(getResourceIcon(...), props) 零改动。

export type ResourceGlyphProps = { className?: string; strokeWidth?: number };
export type ResourceGlyph = FC<ResourceGlyphProps>;

/** 资源图标共享外壳 —— 实心墨层,无描边。 */
function Glyph({ children, className }: { children: ReactNode; className?: string }) {
  return (
    <svg viewBox="0 0 24 24" fill="currentColor" stroke="none" className={className} aria-hidden>
      {children}
    </svg>
  );
}

/** 主动作 —— 实心圆章 + 针孔负空间(12px 下孔闭合成纯圆盘,轮廓不受影响)。 */
const MainActionGlyph: ResourceGlyph = ({ className }) => (
  <Glyph className={className}>
    <path
      fillRule="evenodd"
      d="M12 3.8 a8.2 8.2 0 1 0 .01 0 Z
         M12 9.8 a2.2 2.2 0 1 1 -.01 0 Z"
    />
  </Glyph>
);

/** 附赠动作 —— 实心三角(尖朝上)+ 针孔。 */
const BonusActionGlyph: ResourceGlyph = ({ className }) => (
  <Glyph className={className}>
    <path
      fillRule="evenodd"
      d="M12 3.2 L20.6 19.4 H3.4 Z
         M12 11.2 a2 2 0 1 1 -.01 0 Z"
    />
  </Glyph>
);

/** 反应 —— 实心盾 + 针孔。 */
const ReactionGlyph: ResourceGlyph = ({ className }) => (
  <Glyph className={className}>
    <path
      fillRule="evenodd"
      d="M12 3 C15 4.7 17.5 5.4 19.5 5.6 V12 C19.5 16.9 16.5 19.8 12 21.6 C7.5 19.8 4.5 16.9 4.5 12 V5.6 C6.5 5.4 9 4.7 12 3 Z
         M12 9 a2 2 0 1 1 -.01 0 Z"
    />
  </Glyph>
);

/** 移动力 —— 双箭头 »。 */
const MovementGlyph: ResourceGlyph = ({ className }) => (
  <Glyph className={className}>
    <path d="M4.4 5.4 L11 12 L4.4 18.6 H7.6 L14.2 12 L7.6 5.4 Z" />
    <path d="M11.4 5.4 L18 12 L11.4 18.6 H14.6 L21.2 12 L14.6 5.4 Z" />
  </Glyph>
);

/** 子弹 —— 弹壳侧影(弹头 + 底缘)。 */
const AmmoGlyph: ResourceGlyph = ({ className }) => (
  <Glyph className={className}>
    <path d="M12 2.4 C15 4.2 15.7 6.6 15.7 9 V16.2 H8.3 V9 C8.3 6.6 9 4.2 12 2.4 Z" />
    <path d="M7.7 17.4 H16.3 V19.4 H7.7 Z" />
  </Glyph>
);

/** 法术环(统一/伞 tag)—— 圆环,"环"的字面义。 */
const SpellRingGlyph: ResourceGlyph = ({ className }) => (
  <Glyph className={className}>
    <path
      fillRule="evenodd"
      d="M12 3.6 a8.4 8.4 0 1 0 .01 0 Z
         M12 6.4 a5.6 5.6 0 1 1 -.01 0 Z"
    />
  </Glyph>
);

/** 一至四环 —— 环 + 环下计数点。工厂:pips 数量即环数。 */
function makeSlotRingGlyph(pips: number): ResourceGlyph {
  const SlotRing: ResourceGlyph = ({ className }) => (
    <Glyph className={className}>
      <path
        fillRule="evenodd"
        d="M12 3.4 a7.2 7.2 0 1 0 .01 0 Z
           M12 5.8 a4.8 4.8 0 1 1 -.01 0 Z"
      />
      {Array.from({ length: pips }, (_, i) => (
        <circle key={i} cx={12 + (i - (pips - 1) / 2) * 3.6} cy={20.4} r={1.4} />
      ))}
    </Glyph>
  );
  return SlotRing;
}
const SpellRing1Glyph = makeSlotRingGlyph(1);
const SpellRing2Glyph = makeSlotRingGlyph(2);
const SpellRing3Glyph = makeSlotRingGlyph(3);
const SpellRing4Glyph = makeSlotRingGlyph(4);

/** 虚法术位 —— 断环(四段虚线环,会消逝的环)。描边实现,随 viewBox 等比缩放。 */
const VirtualRingGlyph: ResourceGlyph = ({ className }) => (
  <Glyph className={className}>
    <circle
      cx={12} cy={12} r={7}
      fill="none" stroke="currentColor" strokeWidth={2.7}
      strokeDasharray="5.2 5.8" strokeLinecap="round"
    />
  </Glyph>
);

/** 基础资源伞 tag —— 四点簇("一捆基础资源";cost 直接配父 tag 时的 chip 展示)。 */
const BasicBundleGlyph: ResourceGlyph = ({ className }) => (
  <Glyph className={className}>
    <circle cx={9.7} cy={9.7} r={2.3} />
    <circle cx={14.3} cy={9.7} r={2.3} />
    <circle cx={9.7} cy={14.3} r={2.3} />
    <circle cx={14.3} cy={14.3} r={2.3} />
  </Glyph>
);

/** 未注册资源的 fallback —— 菱形印记。 */
const ResourceSigilGlyph: ResourceGlyph = ({ className }) => (
  <Glyph className={className}>
    <path
      fillRule="evenodd"
      d="M12 3.4 L20.2 12 L12 20.6 L3.8 12 Z
         M12 10 a2 2 0 1 1 -.01 0 Z"
    />
  </Glyph>
);

// ─── Attribute glyphs(版画剪影,属性面板 14px)────────────────────
//
// 函数声明(非 const 箭头)—— 靠 hoisting 让文件上方的 ATTRIBUTE_ICONS 引用得到。

/** 护甲厚度 —— 倒角装甲板 + 负空间铆钉(呼应穿深族的板材意象;14px 下铆钉优雅闭合)。 */
function ArmorThicknessGlyph({ className }: ResourceGlyphProps) {
  return (
    <Glyph className={className}>
      <path
        fillRule="evenodd"
        d="M6.6 3.8 H17.4 L20.2 6.6 V17.4 L17.4 20.2 H6.6 L3.8 17.4 V6.6 Z
           M7.5 6.5 a1 1 0 1 0 .01 0 Z
           M16.5 6.5 a1 1 0 1 0 .01 0 Z
           M7.5 15.5 a1 1 0 1 0 .01 0 Z
           M16.5 15.5 a1 1 0 1 0 .01 0 Z"
      />
    </Glyph>
  );
}

/** 夜视范围 —— 猫眼(杏仁眼形 + 负空间竖狭瞳,夜行生物之眼)。竖瞳刻意与心灵/揭示族的
    圆瞳眼(shell-psychic / DefaultSigil)拉开 —— 那边是"被看见/被揭示",这边是"在黑暗中看"。 */
function NightVisionGlyph({ className }: ResourceGlyphProps) {
  return (
    <Glyph className={className}>
      <path
        fillRule="evenodd"
        d="M3.2 12 Q12 5 20.8 12 Q12 19 3.2 12 Z
           M12 8 C13.3 9.3 13.3 14.7 12 16 C10.7 14.7 10.7 9.3 12 8 Z"
      />
    </Glyph>
  );
}

// ─── Resource icon map ───────────────────────────────────────────
//
// Key = GameplayTag 字符串,跟 C++ FBattleResourceTags 一一对应(新增 / 改名两边同步)。
// Tag tree:配子 tag 优先生效,没配则回溯父 tag(L3/L4 已前置注册,资产侧启用即生效)。

const RESOURCE_ICONS: Record<string, ResourceGlyph> = {
  'Resource.BasicResource':             BasicBundleGlyph,
  'Resource.BasicResource.MainAction':  MainActionGlyph,
  'Resource.BasicResource.BonusAction': BonusActionGlyph,
  'Resource.BasicResource.Reaction':    ReactionGlyph,
  'Resource.BasicResource.Movement':    MovementGlyph,
  'Resource.Ammo':                      AmmoGlyph,
  'Resource.SpellSlot':                 SpellRingGlyph,
  'Resource.SpellSlot.L1':              SpellRing1Glyph,
  'Resource.SpellSlot.L2':              SpellRing2Glyph,
  'Resource.SpellSlot.L3':              SpellRing3Glyph,
  'Resource.SpellSlot.L4':              SpellRing4Glyph,
  'Resource.SpellSlot.Virtual':         VirtualRingGlyph,
};

/**
 * 拿 resource icon。Tag tree 回溯:`Resource.SpellSlot.L1` 没单独配时,试 `Resource.SpellSlot`。
 * 都没配返 fallback(菱形印记)。
 */
export function getResourceIcon(resourceTag: string): ResourceGlyph {
  let key: string | undefined = resourceTag;
  while (key) {
    const hit = RESOURCE_ICONS[key];
    if (hit) return hit;
    const dot = key.lastIndexOf('.');
    if (dot < 0) break;
    key = key.slice(0, dot);
  }
  return ResourceSigilGlyph;
}

// ─── Buff tone icon ───────────────────────────────────────────────
//
// Buff 在面板列表里用一个小圆点标记 tone(positive 金,negative 红,neutral 灰)——
// 见 BattleUnitStatusPanel 的 buff chip。这里不返 icon,而是返"tone 对应的色"+ 说明
// 一起暴露,call site 拼接即可。

export interface BuffToneStyle {
  /** Tailwind 文字 / 背景类 */
  textClass: string;
  bgClass: string;
  /** dot 中心高亮色(给 box-shadow 用) */
  glow: string;
  /** 跟 Crown 等可能用得到的"等级感"icon,后期 boss buff 之类的可借这条入口 */
  icon: LucideIcon;
}

const TONE_STYLES: Record<'positive' | 'negative' | 'neutral', BuffToneStyle> = {
  positive: { textClass: 'text-emerald-300', bgClass: 'bg-emerald-500/15',  glow: 'rgba(16,185,129,0.6)', icon: Sparkles },
  negative: { textClass: 'text-rose-300',    bgClass: 'bg-rose-500/15',     glow: 'rgba(244,63,94,0.6)',  icon: Flame },
  neutral:  { textClass: 'text-parchment',   bgClass: 'bg-panel-light',     glow: 'rgba(212,168,67,0.5)', icon: Crown },
};

export function getBuffToneStyle(tone: 'positive' | 'negative' | 'neutral'): BuffToneStyle {
  return TONE_STYLES[tone];
}

// ─── 直接导出常用 icon,降低 call site 的 import 噪音 ─────────────
export { Heart, Droplet, Swords, Shield, Sparkles, Zap, Hourglass, Flame, Snowflake, Crown, Hexagon, Star, Footprints };
