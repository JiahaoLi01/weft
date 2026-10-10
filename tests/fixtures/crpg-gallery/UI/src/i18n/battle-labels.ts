import { useCallback } from "react";
import { useI18nStore } from "@/stores/i18n-store";
import type { Locale } from "@/i18n";

/**
 * Battle Labels —— 战斗中频繁出现、需要本地化的"短标签"集中地。
 *
 * 涵盖两类:
 *   1. **属性 (Attribute)** —— `FGameplayAttributeField` 的 PropertyName 部分。C++ 推过来
 *      的全名格式是 `"ClassName.PropertyName"`(如 `CRPGGameplayAttributeData.MaxHealth`),
 *      查表前剥掉前缀。
 *   2. **资源 (Resource)** —— `FBattleResource::ResourceTag` 的完整 tag 字符串
 *      (如 `Resource.BasicResource.MainAction` / `Resource.SpellSlot.L1` / `Resource.SpellSlot.Virtual`)。
 *      跟 C++ FBattleResourceTags namespace 的 native tag 一一对应。
 *
 * --------------------------------------------------------------------------------
 * 为什么放在 `i18n/`(而非 `config/`):
 *   项目约定 —— **`i18n/` 文件夹下所有内容都是"需要本地化的"**,反过来意味着 `config/`
 *   下默认是 non-localized 配置(单语言),翻译扫描只看 `i18n/`,不漏 `config/` 零散文件。
 *   分类做成目录级 invariant,加新东西时位置很明确。
 *
 * 为什么单独一个文件,不塞进同目录 `locales/*.ts` 的扁平 key→string 表:
 *   - locale 文件每条 entry 是 1 行(一种语言),这里每条 entry 是 N 行(所有语言),
 *     视觉上一个属性的所有翻译聚合在一起,翻译师改一处不会漏改别的语言
 *   - 战斗术语集中起来,新加属性 / 资源时一眼看到"现有都映射了什么",避免重复或漏配
 *   - locale 扁平表不被几十条属性名稀释,settings / menu / dialogue 的 keys 仍然好找
 *
 * 跟 `battle-keywords.ts` (BATTLE_KEYWORDS) 的区别:
 *   - keywords 是给 `<RichText>` 用的术语词典,出 hover tooltip(name + description +
 *     nested links),覆盖 Damage.* / Status.* / Resource.* / Stat.* / Term.* 多种命名空间
 *   - 这里是给 chip / row label 直接显示的短文本,只要"名字"一个字段
 *   - 同一个资源(如 MainAction)在两边可能都有 ——keywords 端用于 hover 完整说明,
 *     labels 端用于 inline 渲染。**值要保持一致**,改时记得两边都同步。
 *
 * --------------------------------------------------------------------------------
 * 添加新属性 / 资源:
 *   1. 在对应表(BATTLE_ATTRIBUTE_LABELS / BATTLE_RESOURCE_LABELS)加一条
 *   2. attribute key 是 raw PropertyName(不带 "ClassName." 前缀);resource key 是完整
 *      GameplayTag 字符串(`Resource.BasicResource.MainAction` 等)。
 *   3. value 必须给出全部 Locale 的翻译;不会的语言至少给个 placeholder,后期翻译师扫一遍
 *
 * 查不到的 id 走 `humanize` fallback:strip 前缀 + 在驼峰处插空格。
 */

// ── Locale 标签表 ────────────────────────────────────────────────

export const BATTLE_ATTRIBUTE_LABELS: Record<string, Record<Locale, string>> = {
  MaxHealth: { "zh-CN": "最大生命", en: "Max Health", ja: "最大HP" },
  MaxMana: { "zh-CN": "最大魔法值", en: "Max Mana", ja: "最大MP" },
  ArmorThickness: { "zh-CN": "护甲厚度", en: "Armor Thickness", ja: "护甲厚度" },
  NightVisionRange: { "zh-CN": "夜视范围", en: "Night Vision Range", ja: "暗視範囲" },
  // CurrentHealth / 临时 attributes 暂未在 UI 暴露,按需加
};

// ── Targeting 失败原因 → 本地化文案 ─────────────────────────
//
// Key 是 C++ `FTargetingFailureTags` 的完整 tag string(`Targeting.Invalid.NoLineOfSight` 等),
// 跟 hover tooltip / reject pulse 推过来的 `reasonTag` 一一对应。查不到走 humanize fallback。
// 翻译 string 短而具体,直接显示在 tooltip 里(玩家瞄准时一眼能看完)。
export const TARGETING_FAILURE_LABELS: Record<string, Record<Locale, string>> = {
  "Targeting.Invalid.None": { "zh-CN": "无", en: "None", ja: "なし" },
  "Targeting.Invalid.NullTarget": {
    "zh-CN": "无目标",
    en: "No Target",
    ja: "対象なし",
  },
  "Targeting.Invalid.NoAbility": {
    "zh-CN": "无技能",
    en: "No Ability",
    ja: "スキルなし",
  },
  "Targeting.Invalid.FilterRejected": {
    "zh-CN": "目标不符合条件",
    en: "Target Rejected",
    ja: "条件不一致",
  },
  "Targeting.Invalid.SelfNotAllowed": {
    "zh-CN": "不能选择自己",
    en: "Cannot Target Self",
    ja: "自身は対象外",
  },
  "Targeting.Invalid.AlreadyTargeted": {
    "zh-CN": "已选过此目标",
    en: "Already Targeted",
    ja: "選択済み",
  },
  "Targeting.Invalid.MaxReached": {
    "zh-CN": "目标数已满",
    en: "Max Targets Reached",
    ja: "対象数上限",
  },
  "Targeting.Invalid.NotAiming": {
    "zh-CN": "不在瞄准期",
    en: "Not Aiming",
    ja: "照準中ではない",
  },
  "Targeting.Invalid.OutOfRange": {
    "zh-CN": "超出范围",
    en: "Out of Range",
    ja: "範囲外",
  },
  "Targeting.Invalid.NoLineOfSight": {
    "zh-CN": "视线被阻挡",
    en: "No Line of Sight",
    ja: "視線が通らない",
  },
  "Targeting.Invalid.WrongFaction": {
    "zh-CN": "阵营不符",
    en: "Wrong Faction",
    ja: "陣営違い",
  },
  "Targeting.Invalid.Dead": {
    "zh-CN": "目标已死亡",
    en: "Target is Dead",
    ja: "対象は死亡している",
  },
  // ── 感知 / 视野(SensorManager 给出的不可见原因)──
  "Targeting.Invalid.Blinded": {
    "zh-CN": "目盲",
    en: "Blinded",
    ja: "盲目",
  },
  "Targeting.Invalid.OutOfSightRange": {
    "zh-CN": "超出视距",
    en: "Out of Sight Range",
    ja: "視界外",
  },
  // ── 细分原因(上面几条的子 tag)——来源:sensor 的 BlockReason 覆写(天气 / 黑暗 / 强光),
  // 或 ISightOccluder 遮挡物自述(烟雾)。C++ FTargetingFailureTags 同名 native tag 一一对应。──
  "Targeting.Invalid.NoLineOfSight.Smoke": {
    "zh-CN": "视线被烟雾阻挡",
    en: "Sight Blocked by Smoke",
    ja: "煙で視線が遮られている",
  },
  "Targeting.Invalid.OutOfSightRange.Weather": {
    "zh-CN": "被恶劣天气荫蔽",
    en: "Veiled by Harsh Weather",
    ja: "悪天候に遮られている",
  },
  "Targeting.Invalid.OutOfSightRange.Darkness": {
    "zh-CN": "躲藏在黑暗中",
    en: "Hidden in Darkness",
    ja: "闇に潜んでいる",
  },
  "Targeting.Invalid.Blinded.Glare": {
    "zh-CN": "强烈的光线使得无法目视",
    en: "Blinded by Intense Light",
    ja: "強烈な光で目視できない",
  },
};

// ── End Turn 按钮文案 ───────────────────────────────────────
//
// 简单 per-locale 文本,跟 ROUND_ANNOUNCEMENT_FORMATTERS 同模式。后续如果想做"结束 [阵营名]
// 的回合"这种动态文案,改成 (factionDisplayName) => string formatter 即可。MVP 静态文本。
export const END_TURN_BUTTON_LABEL: Record<Locale, string> = {
  "zh-CN": "结束轮次",
  en: "End Turn",
  ja: "ターン終了",
};

// ── 回合飘字模板(中 / 英 / 日)─────────────────────────────
//
// "第 N 回合" / "Round N" / "ラウンド N" —— i18n 框架的 t() 不支持 placeholder,这里
// 用 per-locale 的 formatter 函数表达。回合数字始终 ASCII(不本地化为中文数字),
// 跟数字字体(Cinzel Decorative)审美一致。
const ROUND_ANNOUNCEMENT_FORMATTERS: Record<Locale, (round: number) => string> = {
  "zh-CN": (n) => `第 ${n} 回合`,
  en: (n) => `Round ${n}`,
  ja: (n) => `ラウンド ${n}`,
};

// ── 伤害提示标签(modifier shared flag → 本地化短文本)──────────────
//
// 这套 label 给 DamageNumber 飘字下方的小号提示文本用。来源是 modifier 的
// SharedFlagContainer 里的 tag,UI 层把 bool 字段映射回 label。当前覆盖装甲穿透
// 系列(过穿 / 未击穿),后续如 "格挡" / "免疫" / "暴击" 文字版都可以塞进来。
//
// 后续如果换 icon:这张表可以扩成 `{ label, iconKey }` 形式,渲染端按情况二选一。
// 暂时只有文本,刚够设计师快速反馈穿透系统行为。
export const DAMAGE_FLAG_LABELS: Record<string, Record<Locale, string>> = {
  OverPenetrated: { "zh-CN": "穿透", en: "Penetrated", ja: "貫通" },
  FailedPenetrate: { "zh-CN": "未击穿", en: "Failed to Penetrate", ja: "貫通失敗" },
};

// ── 异常状态 flag → 本地化短文本(增删飘字用)──────────────────────
//
// Key 是 abnormal flag tag 的 leaf 名(C++ `Battle.AbnormalFlag.*` 的最后一段,如 "Reveal" /
// "Unmovable"),跟 abnormal-flags.tsx 的 icon 表同 key。给 `abnormal-flag` floating UI 的飘字
// 显示「状态名」用(icon + 名字一起飘上来 / 划掉)。
//
// 跟 DAMAGE_FLAG_LABELS 同策略:**未配置返 null,不走 humanize** —— 缺翻译时飘字只显示 icon,
// 不会在中文 UI 里蹦出 "Unmovable" 这种没翻译的英文。加新 flag:C++ 定义 tag → 这里补一行。
export const ABNORMAL_FLAG_LABELS: Record<string, Record<Locale, string>> = {
  Reveal: { "zh-CN": "标记", en: "Revealed", ja: "看破" },
  Unmovable: { "zh-CN": "定身", en: "Rooted", ja: "拘束" },
  Blind: { "zh-CN": "目盲", en: "Blinded", ja: "盲目" },
};

// Key 是 C++ ResourceTag 的完整字符串(`FGameplayTag::ToString()` 输出),跟
// `FBattleResourceTags::Resource_*` native tag 一一对应。新增 / 改名 tag 时两边同步。
//
// **2026-XX 改名**:基础动作经济资源加了 `BasicResource` 伞 tag —— `Resource.MainAction`
// → `Resource.BasicResource.MainAction`(等)。Resource.BasicResource 父 tag 用于"回合开始
// 时自然恢复"等批量规则。UI label 跟着改 key,显示文案不变。
export const BATTLE_RESOURCE_LABELS: Record<string, Record<Locale, string>> = {
  // 基础资源伞 tag —— cost 配父 tag 时 chip 显示 "基础资源" 占位;实际消耗走 partial-match 路径
  // 命中具体子桶(L1 / MainAction 等)。
  "Resource.BasicResource": {
    "zh-CN": "基础资源",
    en: "Basic Resource",
    ja: "基本リソース",
  },
  // 动作经济 (D&D-like)
  "Resource.BasicResource.MainAction": {
    "zh-CN": "主动作",
    en: "Main Action",
    ja: "メインアクション",
  },
  "Resource.BasicResource.BonusAction": {
    "zh-CN": "附赠动作",
    en: "Bonus Action",
    ja: "ボーナスアクション",
  },
  "Resource.BasicResource.Reaction": { "zh-CN": "反应", en: "Reaction", ja: "リアクション" },
  "Resource.BasicResource.Movement": {
    "zh-CN": "移动力",
    en: "Movement",
    ja: "移動力",
  },
  // 法术位树 —— 父 tag(Resource.SpellSlot)在 cost chip 展示;子 tag 在 resource pill
  // 展示每个具体桶的当前 / 上限。
  "Resource.SpellSlot": { "zh-CN": "法术位", en: "Spell Slot", ja: "呪文枠" },
  "Resource.SpellSlot.L1": {
    "zh-CN": "一环法术位",
    en: "Spell Slot I",
    ja: "1階呪文枠",
  },
  "Resource.SpellSlot.L2": {
    "zh-CN": "二环法术位",
    en: "Spell Slot II",
    ja: "2階呪文枠",
  },
  "Resource.SpellSlot.Virtual": {
    "zh-CN": "虚法术位",
    en: "Virtual Slot",
    ja: "仮呪文枠",
  },
};

// ── Internal helpers ────────────────────────────────────────────

/**
 * Fallback humanization —— 未在标签表中的 id 走这个。
 *
 *   "CRPGGameplayAttributeData.MaxHealth" → "Max Health"        (attribute:strip 前缀,驼峰拆词)
 *   "MaxHealth"                            → "Max Health"
 *   "Resource.BasicResource.MainAction"    → "Basic Resource Main Action"   (resource:strip "Resource." 命名空间)
 *   "Resource.SpellSlot.L1"                → "Spell Slot L1"               (保留 namespace 之后的段,拆词)
 *
 * Resource tag 走"strip 顶层命名空间 + dots → spaces + 驼峰拆词"路径:把 tag 树后半段
 * 当成有层级的标签拼成一行。labels 表是权威,humanize 只为漏配兜底。
 */
function humanize(raw: string): string {
  // Resource.* 走 namespace-aware 路径
  if (raw.startsWith("Resource.")) {
    const tail = raw.slice("Resource.".length); // "SpellSlot.L1"
    return tail.replace(/\./g, " ").replace(/([a-z])([A-Z])/g, "$1 $2");
  }
  // Attribute / 其它走"取最后一段 + 驼峰拆词"老路径
  const dotIdx = raw.lastIndexOf(".");
  const stripped = dotIdx >= 0 ? raw.slice(dotIdx + 1) : raw;
  return stripped.replace(/([a-z])([A-Z])/g, "$1 $2");
}

/** Strip "ClassName." 前缀,只留最后一段。属性 id 用得到。 */
function stripPrefix(raw: string): string {
  const dotIdx = raw.lastIndexOf(".");
  return dotIdx >= 0 ? raw.slice(dotIdx + 1) : raw;
}

// ── Hooks ───────────────────────────────────────────────────────

/**
 * 返回 `(rawId) => label` 函数,跟当前 locale 绑定。组件 mount 时 / locale 切换时重建。
 *
 * 用法:
 *   const getAttrLabel = useAttributeLabel();
 *   ...
 *   <span>{getAttrLabel(mod.attribute)}</span>
 *
 * 返回函数而不是直接返回 label,是为了在 `useMemo(() => buffs.map(...))` 这种批量场景里
 * 复用同一个 getter,避免每个 buff 都 useAttributeLabel 一次(那会违反 hook 规则)。
 */
export function useAttributeLabel() {
  const locale = useI18nStore((s) => s.locale);
  return useCallback(
    (rawId: string): string => {
      const key = stripPrefix(rawId);
      return BATTLE_ATTRIBUTE_LABELS[key]?.[locale] ?? humanize(rawId);
    },
    [locale],
  );
}

/**
 * 返回 `(rawId) => label` 函数 —— 资源版。同 `useAttributeLabel` 设计。
 *
 * 资源 id 是完整 tag 字符串(如 `Resource.SpellSlot.L1`),作为 key 直接查表;查不到时
 * humanize 走 namespace-aware 兜底(见上)。
 */
export function useResourceLabel() {
  const locale = useI18nStore((s) => s.locale);
  return useCallback(
    (rawId: string): string =>
      BATTLE_RESOURCE_LABELS[rawId]?.[locale] ?? humanize(rawId),
    [locale],
  );
}

/**
 * 返回 `(flagKey) => label | null` 函数 —— damage 提示标签版。
 *
 * 跟 attr / resource 版的差异:**未配置返 null,不走 humanize**。这是有意的 ——
 * humanize("OverPenetrated") = "Over Penetrated",直接渲染会出现没翻译的英文兜底,
 * 玩家可能在中文 UI 里突然看到英文短语,反而出戏。还不如不显示。
 *
 * 渲染端 `if (label) { <Tag>{label}</Tag> }` 即可,缺翻译的安静失败。
 */
export function useDamageFlagLabel() {
  const locale = useI18nStore((s) => s.locale);
  return useCallback(
    (flagKey: string): string | null =>
      DAMAGE_FLAG_LABELS[flagKey]?.[locale] ?? null,
    [locale],
  );
}

/**
 * 返回 `(flagLeaf) => label | null` —— 异常状态名版。flagLeaf 是 tag leaf 名("Reveal" /
 * "Unmovable")。同 damage flag 版:**未配置返 null,不走 humanize**,渲染端缺翻译时只显示 icon。
 */
export function useAbnormalFlagLabel() {
  const locale = useI18nStore((s) => s.locale);
  return useCallback(
    (flagLeaf: string): string | null =>
      ABNORMAL_FLAG_LABELS[flagLeaf]?.[locale] ?? null,
    [locale],
  );
}

/**
 * 返回 `(round: number) => "第 N 回合" / "Round N" / "ラウンド N"`,跟当前 locale 绑定。
 * 用 closure 而不是直接返字符串,让组件可以在 round 数字变化时直接调,不用每次重新订阅 store。
 */
export function useRoundAnnouncementText() {
  const locale = useI18nStore((s) => s.locale);
  return useCallback(
    (round: number): string => ROUND_ANNOUNCEMENT_FORMATTERS[locale](round),
    [locale],
  );
}

/** 当前 locale 的 End Turn 按钮文案。 */
export function useEndTurnLabel(): string {
  const locale = useI18nStore((s) => s.locale);
  return END_TURN_BUTTON_LABEL[locale];
}

/**
 * 返回 `(reasonTag) => label` 函数。reasonTag 是完整 GameplayTag string;查不到时走 humanize
 * (strip "Targeting.Invalid." 命名空间)。
 *
 * 用法:hover tooltip 显示"无法选中:{label}";reject pulse 复用同函数显示失败原因。
 */
export function useTargetingFailureLabel() {
  const locale = useI18nStore((s) => s.locale);
  return useCallback(
    (reasonTag: string): string =>
      TARGETING_FAILURE_LABELS[reasonTag]?.[locale] ?? humanize(reasonTag),
    [locale],
  );
}
