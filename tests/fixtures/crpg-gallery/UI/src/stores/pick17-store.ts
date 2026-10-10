import { create } from 'zustand';

/**
 * Pick 17 演示模式 —— 开局技能构筑选择的数据 store。
 *
 * C++ `UPick17BattleSchema::PushDraftToUI` 在战斗开局推 `setPick17Draft`(App.tsx 全局
 * 注册 handler 写进来),随后 PushUI('pick17') 拉起 `Pick17Draft` 页。玩家的勾选状态是
 * 页面本地 state(不进 store —— 只有确认那一下才回传 C++,中途状态无人消费)。
 *
 * 确认后页面自行 pop,draft 保留(理论上同场战斗不会推第二次;下场战斗开局会被新
 * payload 覆盖)。
 */

/** 父技能的变体(池子只配父技能,买父带全家;卡上**展开**显示,玩家选购前可读)。 */
export interface Pick17Variant {
  name: string;
  /** 空串 = 未配,UI 回退父技能图标(同族变体沿用父纹章的既有语义)。 */
  icon?: string;
  /** 变体描述(RichText 语法同技能描述)。 */
  description?: string;
}

/** 单个候选技能 —— 与 C++ FPick17AbilityOption 的序列化对齐。 */
export interface Pick17Option {
  /** FAbilityDefinitionId(资产 FName)。确认时按此回传。 */
  id: string;
  name: string;
  description: string;
  /** SVG 图标注册表 key(ability-icons.tsx)。空 = 默认纹章。 */
  icon?: string;
  /** 占用点数(共享预算扣费额)。 */
  points: number;
  /** 稀有度(Meta.Rarity.* 序列化;缺省 = 未标 → 普通显示)。 */
  rarity?: 'common' | 'rare' | 'epic' | 'legendary';
  /**
   * 目录 tag 完整串(`UAbilityDefinition::CategoryTag`,如 "Ability.Category.Spell" /
   * "Ability.Category.Spell.Evocation")。UI 按根映射中文分组、层级细分;缺省归「其他」。
   */
  category?: string;
  /** 父技能的变体列表(展示;确认仍按父技能 id 回传)。 */
  variants?: Pick17Variant[];
}

export interface Pick17Character {
  personaId: string;
  name: string;
  options: Pick17Option[];
}

export interface Pick17Draft {
  /** 全队共享的总点数预算(典型 17)。 */
  totalPoints: number;
  characters: Pick17Character[];
}

interface Pick17State {
  draft: Pick17Draft | null;
  setDraft: (draft: Pick17Draft | null) => void;
}

export const usePick17Store = create<Pick17State>((set) => ({
  draft: null,
  setDraft: (draft) => set({ draft }),
}));
