import type { KeywordDef } from "@/components/NestedTooltip";

/**
 * 战斗术语库 —— BattleUnitPanel 技能描述里的 `{keyword.id}` 占位符在这里查 KeywordDef,
 * 解析 + 渲染由 `<RichText>` 完成。
 *
 * --------------------------------------------------------------------------------
 * 命名约定(强约束,跟 UE GameplayTag 风格对齐):
 *
 *   Damage.*   伤害类型              `Damage.Fire`, `Damage.Arcane`
 *   Status.*   状态效果 (buff/debuff) `Status.Stunned`, `Status.Bleeding`
 *   Resource.* 战斗资源              `Resource.BasicResource.MainAction`, `Resource.SpellSlot.L1`
 *   Stat.*     属性 / 派生数值        `Stat.Crit`, `Stat.AC`
 *   Term.*     其他规则术语           `Term.OpportunityAttack`
 *
 * --------------------------------------------------------------------------------
 * 现在的方案 (战斗 demo 阶段):
 *   - 纯 JS 写死,改文案不要 C++ rebuild。
 *   - i18n 暂未接 —— 文案直接是中文。后面接 useT() 时把 name/description 改成 LocaleKeys。
 *   - id 由这里这个文件 own。C++ ability definition 的 description 字段可以引用任意 id,
 *     找不到时 RichText 会渲染 `[id]` 灰字 —— 漏配立即可见,不会静默失败。
 *
 * 未来要迁移到 UE DataAsset 时 (设计师友好性 / mod 支持触发):
 *   - 在 C++ 加 `UBattleKeywordsDataAsset`,WebUISubsystem 启动时 `CallUI('__keywords_set', ...)`
 *   - 这里改成订阅一个 store,把 BATTLE_KEYWORDS 从模块常量改成 store-backed Map
 *   - 渲染层 (RichText) 不变 —— 它只认 Map<string, KeywordDef>,不在乎数据源
 *
 * --------------------------------------------------------------------------------
 * Tooltip category 的选取:
 *   - 默认 'term'              单一规则术语 (默认 wiki 样式)
 *   - 'status'                 状态效果 (左侧色条 + icon)
 *   - 'equipment' / 'spell'    rarity 边框 + smoke 背景 + stats —— 留给装备/法术卡
 *
 * 颜色用 Tailwind 文本色 class 字符串 (e.g. 'text-red-400'),NestedTooltip 内部会把它转成
 * 边框 / 强调色用。
 */

export const BATTLE_KEYWORDS_LIST: KeywordDef[] = [
  // ── Damage.* ────────────────────────────────────────────────────────
  {
    id: "Damage.Fire",
    name: "火焰伤害",
    description: "元素类伤害,描述一切灼伤。",
    color: "text-orange-400",
  },
  {
    id: "Damage.Psychic",
    name: "心灵伤害",
    description:
      "元素类伤害，描述那些沉重的心灵创伤与无可避免的苦痛，几乎无法被任何形式的防御阻挡。",
    color: "text-fuchsia-400",
  },
  {
    id: "Damage.Arcane",
    name: "奥术伤害",
    description: "纯魔法伤害,无视物理护甲;只被魔抗减免。",
    color: "text-indigo-400",
  },
  {
    id: "Damage.Physical",
    name: "物理伤害",
    description: "普通武器或拳脚造成的伤害,会被 {Stat.AC} 减免。",
    color: "text-zinc-300",
  },
  {
    id: "Damage.Piercing",
    name: "穿刺伤害",
    description: "普通武器或拳脚造成的伤害,会被 {Stat.AC} 减免。",
    color: "text-amber-400",
  },
  {
    id: "Damage.Bludgeoning",
    name: "钝击伤害",
    description: "普通武器或拳脚造成的伤害,会被 {Stat.AC} 减免。",
    color: "text-lime-400",
  },
  {
    id: "Damage.Thunder",
    name: "雷霆伤害",
    description: "描述因雷击造成的灼伤与轰隆作响的雷声造成的伤害。",
    color: "text-sky-400",
  },
  {
    id: "Damage.Poison",
    name: "毒素伤害",
    description: "描述因神经毒素，血液毒素，坏死毒素等造成的伤害。",
    color: "text-emerald-400",
  },
  {
    id: "Damage.Radiant",
    name: "光耀伤害",
    description: "描述因强光造成的灼伤与其他次生伤害。",
    color: "text-amber-100",
  },

  // ── Status.* ────────────────────────────────────────────────────────
  {
    id: "Status.Stunned",
    name: "眩晕",
    description:
      "目标无法移动也无法行动。受到伤害不会解除。期间所有针对该目标的攻击命中获得优势。",
    color: "text-yellow-400",
    category: "status",
    icon: "⚡",
  },
  {
    id: "Status.Burning",
    name: "燃烧",
    description: "回合结束时受到 {Damage.Fire}。可被水系技能或翻滚动作驱散。",
    color: "text-red-400",
    category: "status",
    icon: "\u{1F525}",
  },
  {
    id: "Status.Concentration",
    name: "专注",
    description:
      "维持中的法术需要 {Status.Concentration}。受到伤害时需要进行豁免,失败会中断。一个施法者同一时间只能维持一个法术。",
    color: "text-sky-400",
    category: "status",
  },
  {
    id: "Status.Bleeding",
    name: "流血",
    description:
      "每回合开始时受到 {Damage.Physical}。多层叠加。受治疗效果立即清除。",
    color: "text-rose-400",
    category: "status",
    icon: "\u{1FA78}",
  },

  // ── Resource.* ──────────────────────────────────────────────────────
  // 基础动作经济资源走 `Resource.BasicResource.*` 三环命名 —— 父 tag 用于"回合开始自然
  // 恢复"等批量规则,子 tag 表达具体动作类型。跟 C++ FBattleResourceTags 同步。
  {
    id: "Resource.BasicResource",
    name: "基础资源",
    description:
      "回合制基础动作经济总称(主动作 / 附赠动作 / 反应)。大多数战斗在回合开始时全部刷新。",
    color: "text-amber-200",
  },
  {
    id: "Resource.BasicResource.MainAction",
    name: "主动作",
    description: "每回合一次的主要行动,大多数主动技能消耗。回合开始时刷新。",
    color: "text-amber-400",
  },
  {
    id: "Resource.BasicResource.BonusAction",
    name: "附赠动作",
    description:
      "每回合一次的额外小动作 (例如二次攻击 / 闪现 / 重击)。回合开始时刷新。",
    color: "text-amber-300",
  },
  {
    id: "Resource.BasicResource.Reaction",
    name: "反应",
    description:
      "每回合一次,可在他人回合触发(例如 {Term.OpportunityAttack} / 法术反制)。回合开始时刷新。",
    color: "text-violet-400",
  },
  {
    id: "Resource.BasicResource.Movement",
    name: "移动力",
    description:
      "本回合可走过的总路径长度(单位 cm,UI 显示为米)。寻路系统计算,特殊地形(沼泽 / 难地)会增加单位距离的 cost。回合开始时刷新。",
    color: "text-cyan-300",
  },
  // 法术位树:Resource.SpellSlot 是伞 tag(cost 配此可消耗任意子类型);子 tag 表达具体桶。
  // 跟 C++ FBattleResourceTags::Resource_SpellSlot* native tag 一一对应。
  {
    id: "Resource.SpellSlot",
    name: "法术位",
    description:
      "通用法术位 —— 技能配 cost 为 {Resource.SpellSlot} 时可消耗任意子类型({Resource.SpellSlot.L1} / {Resource.SpellSlot.L2} / {Resource.SpellSlot.Virtual}),按配置顺序优先扣除。",
    color: "text-blue-400",
  },
  {
    id: "Resource.SpellSlot.L1",
    name: "一环法术位",
    description: "消耗一环及以上法术位施放 1 环法术。长休时全部恢复。",
    color: "text-blue-300",
  },
  {
    id: "Resource.SpellSlot.L2",
    name: "二环法术位",
    description: "消耗二环及以上法术位施放 2 环法术。长休时全部恢复。",
    color: "text-blue-400",
  },
  {
    id: "Resource.SpellSlot.Virtual",
    name: "虚法术位",
    description:
      "被临时创生的不稳定法术位，可以超出法术位上限存在，会在战斗结束后消失。拥有者被沉默或者处于法术反制中将导致虚法术位立即消失。",
    color: "text-blue-400",
  },

  // ── Stat.* ──────────────────────────────────────────────────────────
  {
    id: "Stat.AC",
    name: "护甲等级",
    description:
      "攻击 d20 必须 ≥ 目标 AC 才能命中。每点 AC 大约等于 5% 命中率。",
    color: "text-teal-300",
  },
  {
    id: "Stat.Crit",
    name: "暴击",
    description: "d20 投出 20 时触发,伤害骰子数加倍 (修正值不加倍)。",
    color: "text-pink-400",
  },

  // ── Term.* ──────────────────────────────────────────────────────────
  {
    id: "Term.OpportunityAttack",
    name: "借机攻击",
    description:
      "敌方在你的近战范围内主动撤离时触发,消耗 {Resource.BasicResource.Reaction} 进行一次近战攻击。",
    color: "text-emerald-300",
  },
  {
    id: "Term.Advantage",
    name: "优势",
    description: "d20 投两次取较高值。多个优势来源不叠加。",
    color: "text-emerald-400",
  },
  {
    id: "Term.Disadvantage",
    name: "劣势",
    description:
      "d20 投两次取较低值。多个劣势来源不叠加。同时存在优势和劣势时互相抵消,只投一次。",
    color: "text-stone-400",
  },

  // 远程攻击
  {
    id: "Range.OverPenetrated",
    name: "过穿",
    description:
      "命中目标子弹的最小穿深大于目标的护甲厚度，子弹会穿透目标，并继续向前飞行。继续飞行的子弹穿深会降低。",
    color: "text-teal-400",
  },
  {
    id: "Range.FailedPenetrate",
    name: "未击穿",
    description:
      "命中目标子弹的最大穿深低于目标的护甲厚度，子弹会被目标阻挡，多数情况下无法发挥子弹的全部威力。",
    color: "text-rose-400",
  },
  {
    id: "Range.ArmorPenetration",
    name: "穿深",
    description:
      "子弹能够穿透的护甲厚度，由最小穿深和最大穿深组成，敌人的护甲厚度需要恰好落在最小穿深与最大穿深之间，才能造成最有效的伤害，否则会造成{Range.OverPenetrated}或{Range.FailedPenetrate}。",
    color: "text-amber-400",
  },

  // 异常状态
  {
    id: "Abnormal.Reveal",
    name: "揭示",
    description:
      "这个生物的位置被在场的每一个生物感知，其他生物不需要视线就可以感知其位置。",
    color: "text-purple-400",
  },
  {
    id: "Abnormal.Blind",
    name: "目盲",
    description:
      "这个生物无法通过视觉感知任何其他生物的存在。",
    color: "text-gray-400",
  },

  // 天气
  {
    id: "Weather.Type.Rain",
    name: "降雨",
    description:
      "这个天气描述中雨到大雨，所有生物在这个天气中受到的{Damage.Fire}会降低，但{Damage.Thunder}伤害会提高。",
    color: "text-teal-400",
  },
  {
    id: "Weather.Type.Rain.Thunder",
    name: "雷暴雨",
    description:
      "这个天气描述伴随电闪雷鸣的暴雨，所有生物在这个天气中受到的{Damage.Fire}微乎其微，但将巨额放大承受的{Damage.Thunder}。恶劣的天气使得能见度迅速下降，地面的通行性将严重降低。",
    color: "text-indigo-600",
  },
  {
    id: "Weather.Type.Clear",
    name: "晴天",
    description: "这个天气描述令人愉快的澄澈天空，不会对任何生物施加任何影响。",
    color: "text-amber-400",
  },
];

/**
 * Map 形态 —— `<RichText keywords={BATTLE_KEYWORDS}>` 直接吃这个。
 *
 * 模块加载时一次性 build,运行时 readonly。改文案需要 reload(Vite HMR 会处理)。
 */
export const BATTLE_KEYWORDS: Map<string, KeywordDef> = new Map(
  BATTLE_KEYWORDS_LIST.map((kw) => [kw.id, kw]),
);
