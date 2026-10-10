import { create } from 'zustand';

/**
 * Battle UI store — driven by UE→JS pushes during battle.
 *
 * 当前只承担 SelectedUnit 信息(角色名 + 技能列表)。后续会膨胀(回合顺序、
 * targeting 提示、buff icon 列表等),保持 store 一个、按 slice 分字段即可。
 */

/**
 * 一条技能消耗,跟 C++ `UAbilityCostBattleResource` 序列化的 schema 对齐。
 *
 *   type=resource:typed cost,resourceId + amount 对应 BattleResourceContainer。UI 可以跟
 *                  resources 数组对照算"够不够花",决定 ability 是否置灰。
 *   type=other:   非 resource 类型的 cost(项目以后扩展的别的 UAbilityCostBase 子类),
 *                  只带 description 字符串,UI 当 tooltip 显示,不参与可用性判断。
 */
/**
 * polarity:可选的"效果极性"提示,来自 C++ `UAbilityCostBase::MetaTags`(Meta.Positive /
 * Meta.Negative)。职业 / 装备 hook 把某技能 cost 换成更便宜(positive)或更贵(negative)的一份时,
 * 设计师标这个 tag,UI 用增益绿 / 减益红染色提示玩家"这条消耗被改过"。缺省 = 中性,走默认满足 / 不满足配色。
 */
export type BattleAbilityCost =
  | { type: 'resource'; resourceId: string; amount: number; polarity?: 'positive' | 'negative' }
  | { type: 'other'; description: string; polarity?: 'positive' | 'negative' };

/**
 * 一条技能 requirement,跟 C++ `UAbilityRequirementBase::IsRequirementMet` 在
 * `BuildSelectedBattleUnitJson` 里序列化的 schema 对齐。
 *
 *   met=true:           条件满足。failReason 为空串。
 *   met=false:          条件不满足。failReason 是给玩家看的本地化字符串(例如
 *                       "Required tag Buff.Concentration not present")。UI 用 tooltip
 *                       展示,并把这条 ability 整体置灰(逻辑跟 affordable 一致 —
 *                       requirement 不满足 = 不能点)。
 *
 * 跟 cost 的区别:requirement 是"条件",不消耗;cost 是"消耗"。两者都决定 ability
 * 能不能 activate(C++ 端 ExecuteAbility 也是 Requirement → CheckCost → ApplyCost 顺序)。
 */
export interface BattleAbilityRequirement {
  met: boolean;
  failReason: string;
}

/**
 * 一个 ability 的变体 —— **变体本身就是另一个 `UAbilityDefinition`**(heavy variant 模型,
 * 父 ability 在 Variants 数组里列变体 ID)。
 *
 * 跟 `BattleAbilityRow` schema 几乎一致(都含 costs / requirements)—— 因为 variant 在
 * gameplay 层就是 ability,只是不进 container 顶层。UI 用 `costs` 跟 unit 当前 resources
 * 对比算 affordability(跟 SkillCard 同款),`requirements` 任一未满足同样置灰。这样
 * 玩家在变体选择面板里就能看到"哪些变体能用 / 缺什么资源",不用点进去才发现 cost 失败。
 *
 * `id` 是 `FAbilityDefinitionId` 的字符串形式(asset FName),点击时随 abilityClicked
 * payload 一起回传给 C++ 让它 resolve 出 variant ability。
 */
export interface BattleAbilityVariant {
  id: string;
  displayName: string;
  description: string;
  /**
   * SVG 图标注册表 key(`ability-icons.tsx`,对应 C++ `UAbilityDefinition::IconName`)。
   * 空串 / 缺省 = variant 未配置图标,UI 回退父技能图标再回退默认纹章。
   */
  icon?: string;
  costs: BattleAbilityCost[];
  requirements: BattleAbilityRequirement[];
}

export interface BattleAbilityRow {
  id: string;
  displayName: string;
  description: string;
  /** SVG 图标注册表 key,同 `BattleAbilityVariant.icon`。空 = 默认纹章。 */
  icon?: string;
  costs: BattleAbilityCost[];
  requirements: BattleAbilityRequirement[];
  /**
   * 变体列表。空数组(或缺省 → 老 payload 兼容)= 无变体。
   * 有变体时 UI 点击 ability 不直接 ExecuteAbility,而是切到变体选择子面板。
   */
  variants?: BattleAbilityVariant[];
}

/**
 * 一个战斗资源,跟 C++ `FBattleResource` + `FBattleResourceState` 序列化对齐。
 * `id` 是完整 GameplayTag 字符串(如 `Resource.BasicResource.MainAction` / `Resource.SpellSlot.L1`)。
 * 图标不走 payload —— 前端按 id(tag)查 battle-icons.ts 的 RESOURCE_ICONS 注册表(tag tree 回溯)。
 * 一个 resource 就是 (id, current, max) 一条 entry,没有内部分桶 —— 法术位按
 * level 拆成独立资源(`Resource.SpellSlot.L1` / `Resource.SpellSlot.L2` /
 * `Resource.SpellSlot.Virtual` ...),靠 tag tree partial-match 让父 tag cost
 * (`Resource.SpellSlot`)能从子桶取库存。
 */
export interface BattleResource {
  id: string;
  current: number;
  max: number;
}

/**
 * Buff attribute modifier —— 跟 C++ `FGameplayAttributeModifierData` 对齐。
 * C++ 推**原始属性全名**(如 "CRPGGameplayAttributeData.MaxHealth")和 operator 枚举字符串,
 * JS 渲染层负责 humanize(strip 前缀 / 选 +×= 符号 / 格式化数值)。
 */
export type BattleBuffOperator = 'Add' | 'Multi' | 'FinalAdd' | 'FinalMulti' | 'ForceSet';

export interface BattleBuffModifier {
  attribute: string;
  op: BattleBuffOperator;
  value: number;
}

export type BattleBuffLifetimeType = 'Immediately' | 'Limited' | 'Infinity';

/**
 * 一条 buff override group 在 UI 里的表示。`stackCount` 已经包含 stack 模式的层数;
 * displayName 由 C++ `FBuffOverrideGroup::GetDisplayText` 直接生成(Stack 模式带 "× N")。
 */
export interface BattleBuff {
  id: string;
  displayName: string;
  description: string;
  stackCount: number;
  lifetimeType: BattleBuffLifetimeType;
  /**
   * 剩余回合数(**整数**)。单位契约(2026-07-13 反转):CRPGLike tick 管线整体以回合
   * 为单位;取整在 C++ 端一次算好(eps-ceil,eps 与判死同源,"显示的数字 = 还会经历的
   * 衰减回合点数"),UI 不做任何浮点处理。< 0 = 非 Limited 生命周期,不展示倒计时。
   */
  remainingRounds: number;
  modifiers: BattleBuffModifier[];
  /**
   * 显式效果极性(C++ `FBuffData::MetaTags` 的 Meta.Positive / Negative / Special)。存在时
   * **覆盖**面板默认"按属性增减推断"的着色;缺省 = 让 UI 自己按属性推断。
   * 'special' = 非增益非减益的稀有效果,面板走钢印 + 烟雾风格。
   */
  polarity?: 'positive' | 'negative' | 'special';

  /**
   * 稀有度(Meta.Rarity.*,与极性正交 —— buff 通常由带稀有度的装备/天赋赋予)。
   * 缺省 = 未标。当前消费方:special buff 的烟雾按此配色(同 nested tooltip 的
   * rarity 色:common 灰 / rare 蓝 / epic 紫 / legendary 琥珀),未标落回古金。
   */
  rarity?: 'common' | 'rare' | 'epic' | 'legendary';
  /**
   * 仅开发期出现:此 buff 标了 Meta.Hidden,正常不进面板,但被 `UCRPGDeveloperSettings::
   * bForceShowHiddenBuffs` 强制显示。UI 据此打标。正式(Shipping)包永远没有此字段 —— 隐藏 buff 根本不下发。
   */
  hidden?: boolean;
}

/**
 * 一条属性,跟 C++ `UCRPGGameplayAttribute` 的 (BaseValues / CurrentValues) 序列化对齐。
 *
 *   id:      属性全名(`FGameplayAttributeField::AttributeName`),格式 "ClassName.PropertyName"
 *            (例 `CRPGGameplayAttributeData.MaxHealth`)。UI 通过 `useAttributeLabel`
 *            humanize 显示。
 *   base:    BaseValues —— 装备 / 等级 / class 等带来的基础值(不含 buff modifier 链)
 *   current: CurrentValues —— Add / Multi / FinalAdd / FinalMulti 链算完的最终值。
 *            UI 用 base !== current 判定"被 buff 改了",显示原值 → 当前值对比。
 *
 * 注:CurrentHealth(当前生命)**不**在这里,它走 `BattleSelectedUnit.currentHealth`
 * 单独字段(C++ 端 BEAC::Health 是权威源,不经 attribute 系统;UI 跟 MaxHealth 合血条)。
 */
export interface BattleAttribute {
  id: string;
  base: number;
  current: number;
}

export interface BattleSelectedUnit {
  name: string;
  abilities: BattleAbilityRow[];
  resources: BattleResource[];
  buffs: BattleBuff[];
  attributes: BattleAttribute[];
  /** 当前 HP。跟 attributes 中 id="CRPGGameplayAttributeData.MaxHealth" 的 current 一起算血条百分比。 */
  currentHealth: number;
}

/**
 * 技能瞄准期的状态。AbilityController → Ability.Intent.{AimingStarted,TargetAccepted,
 * AimingCancelled,PerformStarted} → WebUISubsystem → setBattleTargeting(C++) → 这里。
 *
 * 面板用 `targeting.active` 切换 body:active 时显示"目标 X/Y",inactive 时显示技能列表。
 */
export interface BattleTargeting {
  active: boolean;
  picked: number;
  required: number;
}

/**
 * 单个阵营的 UI 静态数据 —— C++ UFactionDataSettings 经 setBattleSession 推过来。
 *
 *   tag      —— 完整 GameplayTag string(map key,同 currentTurn.factionTag 同源)
 *   name     —— 本地化显示名(UE 端 FText 在推送时按当前 culture 解析)
 *   emblemId —— 纹章注册表 key,前端 config/faction-emblems.tsx 解析成实际图形;
 *               UE 不传图片数据,未注册 id 走 fallback 纹章
 *   color    —— 可选主题色 hex(空串走默认羊皮纸色)
 */
export interface BattleFactionInfo {
  tag: string;
  name: string;
  emblemId: string;
  color: string;
}

/**
 * Battle session 状态 —— BattleUIDispatcher OnActivated/OnDeactivated 推送。
 * active=true 时 BattleSessionOverlay 挂载战斗常驻控件(End Turn 按钮 / 回合横幅);
 * factions 是阵营静态目录(tag → 显示数据),关闭时清空。
 */
export interface BattleSession {
  active: boolean;
  factions: Record<string, BattleFactionInfo>;
}

/**
 * 当前轮次信息(哪个阵营在行动)。从 C++ Battle.Event.TurnAdvanced 桥接 + SyncCurrentState
 * 开局补推。
 *
 *   factionTag: 完整 GameplayTag string (例 "Faction.Player" / "Faction.Hostile")。
 *               空 string → "没轮次概念"(战斗未开始 / TurnOrder 空)。UI 用来 disabled
 *               End Turn 按钮 + 隐藏指示器。
 *   turnIndex:  TurnOrder 数组下标。debug / 高级 UI 可能用。MVP UI 只读 factionTag。
 */
export interface BattleCurrentTurn {
  factionTag: string;
  turnIndex: number;
}

/**
 * 瞄准期 hover 校验状态。PC.BattleTargetingState 每帧 raycast cursor + ValidateTarget,
 * 节流后 push 到这里。UI 端 `TargetingHoverTooltip` 订阅这个 slice 决定是否渲染 tooltip。
 *
 *   active     —— cursor 是否命中世界(false = 鼠标飞天 / 不在瞄准期 / 战斗外)。false 时 UI 隐藏 tooltip
 *   valid      —— 命中目标是否可选(active=true 时才有意义;active=false 时 false 兜底)
 *   reasonTag  —— valid=false 时的 FTargetingFailureTags 完整 tag string,UI i18n humanize
 *   actorName  —— hover 命中 actor 的名字,可空(Point 类落在 landscape 时空)
 */
export interface BattleTargetingHover {
  active: boolean;
  valid: boolean;
  reasonTag: string;
  actorName: string;
}

/**
 * Reject pulse —— 玩家点了无效目标的一次性反馈信号。`trigger` 自增让 React key 失效从而
 * 重播 shake/flash 动画;`reasonTag` 给 UI 显示具体原因。同一原因反复点也能重播(trigger 在变)。
 *
 * trigger 自增模式 —— React 端拿它当 key / effect 依赖,同内容重推也能重播动画。
 */
export interface BattleTargetingRejectPulse {
  reasonTag: string;
  trigger: number;
}

/**
 * 移动消耗预览 —— BattleMoveController hover 路径变化时(diff 节流)推到这里,UI 端
 * `MovePreviewTooltip` 显示"X.XX m"跟随鼠标。
 *
 *   active     —— hover 路径是否有效。false 时 UI 隐藏 tooltip
 *   reachable  —— 是否在可用 Movement 资源范围内(决定 tooltip 红 / 绿)
 *   costCm     —— 寻路距离 (cm,C++ 端原生单位)。UI 转米显示
 *   availableCm—— caster 当前可用 Movement 资源 (cm,同上)。unreachable 时显示"超出范围 X/Y"
 */
export interface BattleMovePreview {
  active: boolean;
  reachable: boolean;
  costCm: number;
  availableCm: number;
}

interface BattleState {
  /** null 表示当前没选中(C++ 推 {name:null} 时清空 → 面板隐藏) */
  selectedUnit: BattleSelectedUnit | null;
  /** 瞄准期状态。active=false 时面板回到技能列表。 */
  targeting: BattleTargeting;
  /** Battle session 开关 + 阵营目录。active=false 时战斗常驻控件(End Turn / 回合横幅)不挂载。 */
  session: BattleSession;
  /** 当前回合数。setBattleRound 维护(开局 SyncCurrentState 补推 + RoundAdvanced 桥接)。 */
  currentRound: number;
  /** 当前轮次(行动阵营 + 序号)。factionTag 为空 string = 没轮次概念。 */
  currentTurn: BattleCurrentTurn;
  /** 瞄准期 hover 校验。active=false 时 TargetingHoverTooltip 隐藏。 */
  targetingHover: BattleTargetingHover;
  /** 玩家点了无效目标的一次性反馈。null = 没 pending pulse;非空时组件播一次动画后调 clear 清空。 */
  targetingRejectPulse: BattleTargetingRejectPulse | null;
  /** 移动 hover 距离预览。active=false 时 UI 隐藏 tooltip。 */
  movePreview: BattleMovePreview;

  setSelectedUnit: (payload: {
    name: string | null;
    abilities: BattleAbilityRow[];
    resources?: BattleResource[];
    buffs?: BattleBuff[];
    attributes?: BattleAttribute[];
    currentHealth?: number;
  }) => void;
  setTargeting: (payload: BattleTargeting) => void;
  /** Battle session 开关 + 阵营目录(C++ 推数组,这里翻成 tag-keyed map 便于查表)。 */
  setBattleSession: (payload: { active: boolean; factions?: BattleFactionInfo[] }) => void;
  setCurrentRound: (round: number) => void;
  setCurrentTurn: (payload: BattleCurrentTurn) => void;
  setTargetingHover: (payload: BattleTargetingHover) => void;
  /** 推 reject pulse,trigger 自动 +1 让 React 识别"新一条"重播动画。 */
  pushTargetingRejectPulse: (reasonTag: string) => void;
  clearTargetingRejectPulse: () => void;
  setMovePreview: (payload: BattleMovePreview) => void;
}

export const useBattleStore = create<BattleState>((set) => ({
  selectedUnit: null,
  targeting: { active: false, picked: 0, required: 0 },
  session: { active: false, factions: {} },
  currentRound: 0,
  currentTurn: { factionTag: '', turnIndex: 0 },
  targetingHover: { active: false, valid: false, reasonTag: '', actorName: '' },
  targetingRejectPulse: null,
  movePreview: { active: false, reachable: false, costCm: 0, availableCm: 0 },

  setSelectedUnit: (payload) => {
    if (payload.name === null) {
      set({ selectedUnit: null });
      return;
    }
    set({
      selectedUnit: {
        name: payload.name,
        abilities: payload.abilities ?? [],
        resources: payload.resources ?? [],
        buffs: payload.buffs ?? [],
        attributes: payload.attributes ?? [],
        currentHealth: payload.currentHealth ?? 0,
      },
    });
  },

  setTargeting: (payload) => set({ targeting: payload }),

  setBattleSession: (payload) => set(() => {
    const factions: Record<string, BattleFactionInfo> = {};
    for (const f of payload.factions ?? []) {
      if (f?.tag) {
        factions[f.tag] = f;
      }
    }
    return { session: { active: !!payload.active, factions } };
  }),

  setCurrentRound: (round) => set({ currentRound: round }),

  setCurrentTurn: (payload) => set({ currentTurn: payload }),

  setTargetingHover: (payload) => set({ targetingHover: payload }),

  pushTargetingRejectPulse: (reasonTag) => set((state) => ({
    targetingRejectPulse: {
      reasonTag,
      trigger: (state.targetingRejectPulse?.trigger ?? 0) + 1,
    },
  })),

  clearTargetingRejectPulse: () => set({ targetingRejectPulse: null }),

  setMovePreview: (payload) => set({ movePreview: payload }),
}));
