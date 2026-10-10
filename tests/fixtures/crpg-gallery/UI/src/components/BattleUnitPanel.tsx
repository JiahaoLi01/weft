import { createElement, useEffect, useState, type CSSProperties } from 'react';
import { AnimatePresence, motion } from 'framer-motion';
import { setFocus, useFocusable } from '@noriginmedia/norigin-spatial-navigation';
import {
  Crosshair,
  AlertTriangle,
  ChevronRight,
  ArrowLeft,
  BookOpen,
  ScrollText,
  Sparkles,
} from 'lucide-react';
import {
  useBattleStore,
  type BattleAbilityCost,
  type BattleAbilityRequirement,
  type BattleAbilityVariant,
  type BattleResource,
} from '@/stores/battle-store';
import { useUIInputStore } from '@/stores/ui-input-store';
import { sendToUE } from '@/lib/ue-bridge';
import { RichText, usePinnedTooltip } from '@/components/NestedTooltip';
import { BATTLE_KEYWORDS } from '@/config/battle-keywords';
import { BATTLE_RICHTEXT_STYLES } from '@/config/battle-richtext-styles';
import { useResourceLabel } from '@/i18n/battle-labels';
import { BATTLE_PANEL_SURFACE, CornerOrnaments, OrnateDivider, PanelInnerRule, SectionHeader } from '@/components/battle/PanelDecorations';
import { getResourceIcon } from '@/components/battle/battle-icons';
import { AbilityIcon } from '@/components/battle/ability-icons';
import { ANTIQUE_GOLD, amber, brass, garnet, laurel } from '@/config/battle-palette';

/**
 * BattleUnitPanel —— 屏幕右下角的"操作面板"。
 *
 * 视觉构成(瞄准期 vs 非瞄准期分两套):
 *
 * 非瞄准期:
 *   1. 资源条:每种资源一条 pill,小型资源(max ≤ 6,例:法术位)用 dot-grid 显示 N/M
 *      可见,大型资源(max > 6,例:MP)用纯数字 + 进度条
 *   2. 技能列表:每条 ability 一个 card,含 icon 槽 / 名字 / 描述 / cost-chip。Affordable
 *      时点击 / Enter 触发 sendToUE;不 affordable 时灰 + cursor-not-allowed
 *
 * 瞄准期:面板内容切换成大号"目标 X/Y"+ crosshair icon + "Esc 取消" 提示
 *
 * 跟 BattleUnitStatusPanel(左下角)是配对的两个 panel,共享同一个 selected store。
 *
 * 三种交互输入并存:鼠标 hover/click、键盘 Enter、手柄 D-pad/A —— 视觉高亮统一走
 * "isActive = isHovered || focused"。详见每个 SkillCard 内部。
 */

interface Ability {
  id: string;
  displayName: string;
  description?: string;
  /** SVG 图标注册表 key(ability-icons.tsx)。空 = 默认纹章。 */
  icon?: string;
  costs: BattleAbilityCost[];
  requirements: BattleAbilityRequirement[];
  /**
   * 变体列表(可选)。非空 → 点击此 ability 进入变体选择子模式而非直接 ExecuteAbility,
   * 跟 C++ `UAbilityDefinition::HasVariants()` 语义一致。
   */
  variants?: BattleAbilityVariant[];
}

interface SkillCardProps {
  ability: Ability;
  focusKey: string;
  isHovered: boolean;
  isPressed: boolean;
  /**
   * 是否可激活。false 时灰 + 不响应 click / Enter。
   * 由 isAbilityAffordable 算:所有 resource cost 充足 + 所有 requirement met。
   */
  affordable: boolean;
  /**
   * 当前角色的资源池 —— 用来给每个 cost chip 算"本条单独是否满足"。
   * SkillCard 整体 affordable 是 AND-all,但 cost chip 视觉上需要逐条标 ——
   * 要 Ammo+Action 时只缺 Action,Ammo chip 仍该显金,玩家才知道缺哪个。
   */
  resources: BattleResource[];
  onHoverChange: (id: string | null) => void;
  onPressStart: () => void;
  onPressEnd: () => void;
  onActivate: () => void;
}

// ─── Tag tree partial-match(跟 C++ FGameplayTag::MatchesTag 同语义)────────
function matchesTag(tag: string, query: string): boolean {
  if (tag === query) return true;
  return tag.startsWith(query + '.');
}

function getResourceCurrentMatchingTag(resources: BattleResource[], queryTag: string): number {
  let sum = 0;
  for (const res of resources) {
    if (matchesTag(res.id, queryTag)) {
      sum += res.current;
    }
  }
  return sum;
}

/**
 * Ability 是否可激活。两条独立 gate:
 *   1. resource cost 充足(支持 tag tree partial-match,跟 C++ 一致)
 *   2. 所有 requirement.met === true
 *
 * 顺序跟 C++ ExecuteAbility 一致(Requirement → CheckCost)。否则 UI 显示能用、点了 C++
 * 又拒掉,玩家会困惑。
 */
/**
 * 单条 cost 当前能不能被资源池满足。`type=other` 的 cost 永远视为满足(JS 不验证非资源
 * cost,它们是描述性的)。`type=resource` 走跟 C++ 一致的 partial-match 加总比对。
 *
 * 拆这条出来是为了 cost chip UI 能"逐条"标红/标金:之前所有 chip 共享 ability 整体
 * affordable boolean,只要任一条不满足全部变红,玩家看不清"到底缺哪个资源"。
 */
function isCostSatisfied(cost: BattleAbilityCost, resources: BattleResource[]): boolean {
  if (cost.type !== 'resource') return true;
  return getResourceCurrentMatchingTag(resources, cost.resourceId) >= cost.amount;
}

function isAbilityAffordable(
  costs: BattleAbilityCost[],
  requirements: BattleAbilityRequirement[],
  resources: BattleResource[],
): boolean {
  for (const req of requirements) {
    if (!req.met) return false;
  }
  for (const cost of costs) {
    if (!isCostSatisfied(cost, resources)) return false;
  }
  return true;
}

// ─── Card Surface ─────────────────────────────────────────────────
//
// SkillCard / VariantCard 共用的三态卡面:平面墨色递进(idle → 暖琥珀 → 按压加深)
// + 黄铜细线。active 时 border + inset ring 构成双细线,呼应面板外框的双线规 ——
// 维多利亚版画语言:强调靠"加线",不靠发光。之前的 0 0 10px 金色 glow 已移除。

function cardSurface(isActive: boolean, isPressed: boolean, affordable: boolean): CSSProperties {
  const active = isActive && affordable;
  const pressed = isPressed && affordable;
  return {
    background: pressed
      ? 'rgba(50, 38, 21, 0.6)'
      : active
        ? 'rgba(64, 50, 28, 0.55)'
        : 'rgba(36, 28, 18, 0.45)',
    border: active ? `1px solid ${brass(0.8)}` : `1px solid ${brass(0.16)}`,
    boxShadow: active ? `inset 0 0 0 1px ${brass(0.2)}` : 'none',
    borderRadius: 3,
  };
}

// ─── Resource Pill ────────────────────────────────────────────────
//
// 一个资源的可视化卡片。低 max 走 dot-grid,高 max 走数字。

const DOT_GRID_MAX = 6;

interface ResourcePillProps {
  resource: BattleResource;
  label: string;
}

function ResourcePill({ resource, label }: ResourcePillProps) {
  const isEmpty = resource.current === 0;
  const isDotGrid = resource.max > 0 && resource.max <= DOT_GRID_MAX;
  // 用 createElement 而非 `const Icon = ...; <Icon/>` —— `getResourceIcon` 返回的是
  // lucide 库的静态组件引用(stable across renders),但 react-hooks/static-components
  // 规则用语法启发式拦截"render 期取得 component 变量"的写法。createElement 是 React 官方
  // 推荐的 dynamic component 渲染 API,跳出规则的 false-positive。
  const iconEl = createElement(getResourceIcon(resource.id), {
    className: ['size-3.5 shrink-0', isEmpty ? 'text-parchment-dark/40' : 'text-brass'].join(' '),
    strokeWidth: 1.8,
  });

  return (
    <motion.div
      // 资源量变化时 layout 自动重排,scale up 一下做"反馈"
      layout
      whileHover={{ y: -1 }}
      className="flex items-center gap-1.5 px-2 py-1 rounded-[3px] transition-all"
      // 平面铭牌:细线框 + 墨色底,空仓时线框 / 底色都褪淡。glow / 渐变已移除。
      style={
        isEmpty
          ? { border: `1px solid ${brass(0.16)}`, background: 'rgba(30, 23, 15, 0.4)' }
          : { border: `1px solid ${brass(0.45)}`, background: 'rgba(43, 33, 20, 0.55)' }
      }
      title={`${label}: ${resource.current}/${resource.max}`}
    >
      {iconEl}
      {isDotGrid ? (
        // Dot grid:max 个圆点,前 current 个实心黄铜、耗尽的留细线"空槽"——
        // 像怀表面板上的宝石轴承位,填充态一眼可读
        <span className="flex gap-0.5 items-center">
          {Array.from({ length: resource.max }, (_, i) => {
            const filled = i < resource.current;
            return (
              <motion.span
                key={i}
                // 填充态变化时 scale-pulse
                animate={{ scale: filled ? 1 : 0.7, opacity: filled ? 1 : 0.5 }}
                transition={{ duration: 0.18, ease: 'easeOut' }}
                className="size-1.5 rounded-full"
                style={filled ? { background: ANTIQUE_GOLD } : { border: `1px solid ${brass(0.45)}` }}
              />
            );
          })}
        </span>
      ) : (
        // 数字 fallback —— max 太大(MP 这种)不画 dot,直接 N/M
        <span className="text-[10px] tabular-nums font-[family-name:var(--font-subtitle)]">
          <span className={isEmpty ? 'text-parchment-dark/40' : 'text-gold-light font-semibold'}>
            {resource.current}
          </span>
          <span className="text-parchment-dark/40 mx-0.5">/</span>
          <span className="text-parchment-dark">{resource.max}</span>
        </span>
      )}
    </motion.div>
  );
}

// ─── Cost Chip ────────────────────────────────────────────────────
//
// 技能 cost 的小 chip,带 resource icon。"other" cost 直接显示描述文本。

interface CostChipProps {
  cost: BattleAbilityCost;
  /**
   * **本条 cost 自己**是否被资源池满足(由调用方逐条算,通常走 `isCostSatisfied(cost, resources)`)。
   * 跟 ability 整体 affordable 不是一回事 —— 一个 ability 要 1 Ammo + 1 Action 时,Ammo 够但
   * Action 不够,整体 affordable=false,但 Ammo chip 应该照常显金色,Action chip 才该标红,
   * 这样玩家一眼看出"缺的是 Action"。
   */
  satisfied: boolean;
  getResourceLabel: (id: string) => string;
}

function CostChip({ cost, satisfied, getResourceLabel }: CostChipProps) {
  // 极性染色:hook 改过的 cost 标了 Meta.Positive / Negative → 增益绿(laurel)/ debuff 警告琥珀
  // (amber)。debuff 用琥珀而非石榴红,是为了跟"付不起"的禁止红区分开 —— 红只表示"用不了"。
  const polarityStyle: CSSProperties | null =
    cost.polarity === 'positive'
      ? { background: 'rgba(31, 40, 30, 0.6)', border: `1px solid ${laurel(0.5)}`, color: '#a3c4a3' }
      : cost.polarity === 'negative'
        ? { background: 'rgba(44, 31, 14, 0.6)', border: `1px solid ${amber(0.5)}`, color: '#dba05c' }
        : null;

  if (cost.type === 'other') {
    return (
      <span
        className="flex items-center gap-1 text-[10px] px-1.5 py-0.5 rounded-[3px] text-parchment-dark whitespace-nowrap"
        style={polarityStyle ?? { background: 'rgba(40, 31, 20, 0.6)', border: `1px solid ${brass(0.25)}` }}
      >
        <ScrollText className="size-3 shrink-0" strokeWidth={1.8} />
        {cost.description}
      </span>
    );
  }

  // createElement 走 dynamic-icon rendering(同 ResourcePill 注释)。
  const iconEl = createElement(getResourceIcon(cost.resourceId), {
    className: 'size-3 shrink-0',
    strokeWidth: 1.8,
  });
  return (
    <span
      className={[
        'flex items-center gap-1 text-[10px] px-1.5 py-0.5 rounded-[3px] whitespace-nowrap tabular-nums',
        satisfied ? '' : 'line-through',
      ].join(' ')}
      // 可支付性优先:付不起 → 石榴红 + 删除线(polarity 不能盖掉这个最重要的信号)。付得起时再
      // 用 polarity 染色(增益绿 / 减益红),无 polarity 走默认黄铜古金。
      style={
        satisfied
          ? (polarityStyle ?? { background: 'rgba(43, 33, 20, 0.7)', border: `1px solid ${brass(0.45)}`, color: '#d9c08a' })
          : { background: 'rgba(46, 23, 19, 0.6)', border: `1px solid ${garnet(0.5)}`, color: '#cf8e86' }
      }
      title={`${getResourceLabel(cost.resourceId)} × ${cost.amount}`}
    >
      {iconEl}
      <span className="font-semibold">×{cost.amount}</span>
    </span>
  );
}

// ─── Skill Card ───────────────────────────────────────────────────

function SkillCard({
  ability,
  focusKey,
  isHovered,
  isPressed,
  affordable,
  resources,
  onHoverChange,
  onPressStart,
  onPressEnd,
  onActivate,
}: SkillCardProps) {
  const getResourceLabel = useResourceLabel();
  const { ref, focused } = useFocusable({
    focusKey,
    onEnterPress: () => {
      // 不够花的技能 — focus 可以停在上面(看资源消耗 tooltip),但 Enter 不触发
      if (!affordable) return;
      onActivate();
    },
    onFocus: () => onHoverChange(ability.id),
  });

  const isActive = isHovered || focused;
  const unmetReqs = ability.requirements.filter((r) => !r.met);

  return (
    <motion.li
      ref={ref}
      onMouseEnter={() => onHoverChange(ability.id)}
      onMouseLeave={() => {
        onHoverChange(null);
        onPressEnd();
      }}
      onMouseDown={() => { if (affordable) onPressStart(); }}
      onMouseUp={onPressEnd}
      onClick={() => { if (affordable) onActivate(); }}
      // 进出场 stagger(由 ul layoutAnimation 自动接管)+ hover 时 x 微移
      animate={{
        x: isActive && affordable ? 2 : 0,
        scale: isPressed && affordable ? 0.98 : 1,
      }}
      transition={{ duration: 0.12, ease: 'easeOut' }}
      className={[
        'relative px-2.5 py-1.5 select-none',
        'transition-colors duration-150',
        // **不要**用 `filter: grayscale()`(Tailwind `grayscale-*`)做"置灰"效果 ——
        // CSS filter 会让每张 unaffordable 卡建立独立 composite layer + 每帧重新光栅化,
        // 面板入场 fade-in 时(motion.div opacity 0→1 经 ~9 帧),N 张 filtered 卡 ×
        // 9 帧的合成开销在 Ultralight 下直接吃满预算,玩家肉眼看到掉帧。
        //
        // 只用 opacity 就够:opacity 走 GPU 一次性 layer 透明度变换,基本免费。视觉上
        // 加上 cost chip 的石榴红配色 + line-through、req chip 的红底,"不可用"信号
        // 已经很明显,grayscale 是冗余的。
        affordable ? 'cursor-pointer' : 'cursor-not-allowed opacity-50',
      ].join(' ')}
      // 三态卡面(idle / active / pressed)走共享 cardSurface —— 平面墨色 + 黄铜双细线
      style={cardSurface(isActive, isPressed, affordable)}
    >
      {/* 左侧 chevron 指示器 —— active 时出现,做"被选中"提示 */}
      <motion.div
        className="absolute left-0.5 top-1/2 -translate-y-1/2 text-gold-light"
        animate={{ opacity: isActive && affordable ? 1 : 0, x: isActive && affordable ? 0 : -4 }}
        transition={{ duration: 0.12 }}
        aria-hidden
      >
        <ChevronRight className="size-3" strokeWidth={2.5} />
      </motion.div>

      {/* 图标 + 内容双列:图标纵跨"标题行 + 描述"两行,像古书条目的页缘插画。
          图标色跟标题同步三态(active 金亮 / idle 暗金 / 置灰羊皮暗色),线刻风格
          (ability-icons.tsx)靠 currentColor 继承,无需单独配色。 */}
      <div className="flex gap-2 pl-3">
        <AbilityIcon
          name={ability.icon}
          className={[
            'size-6 shrink-0 mt-0.5 transition-colors',
            // 常态全量 gold(不再 /75 压暗)—— 剪影图标大面积实心,亮度低一档整列会发闷;
            // active 仍用更亮的 gold-light 保持三态区分
            isActive && affordable ? 'text-gold-light' : affordable ? 'text-gold' : 'text-parchment-dark',
          ].join(' ')}
        />

        <div className="flex-1 min-w-0">
          {/* 主行:名字 + (变体标记) + cost chips */}
          <div className="flex items-center justify-between gap-2">
            <span className="flex items-center gap-1 min-w-0">
              {/* 标题走 RichText —— 支持 `{style.brand:V&C}` 这类混合字体 token,典型用于步枪 /
                  法器铭牌的维多利亚商品名风格。纯文本(无 token)直接渲染,跟之前等价。
                  外层 span 上的 font-display 作为"非 style 段"的 base font(plain text + 未配置
                  的 style 段都走这里)。 */}
              <span className={[
                'text-sm font-[family-name:var(--font-display)] transition-colors truncate',
                isActive && affordable ? 'text-gold-light' : affordable ? 'text-parchment' : 'text-parchment-dark',
              ].join(' ')}>
                <RichText text={ability.displayName} keywords={BATTLE_KEYWORDS} styles={BATTLE_RICHTEXT_STYLES} />
              </span>
              {/* 变体标记 —— 有 variants 的 ability 显示一个小 Sparkles icon,
                  暗示"点击会展开变体选择",玩家不会以为点错 / 卡了 */}
              {(ability.variants?.length ?? 0) > 0 && (
                <Sparkles
                  className={[
                    'size-3 shrink-0 transition-colors',
                    isActive ? 'text-gold-light' : 'text-gold/60',
                  ].join(' ')}
                  strokeWidth={2}
                  aria-label="has variants"
                />
              )}
            </span>

            {(ability.costs.length > 0 || unmetReqs.length > 0) && (
              <span className="flex items-center gap-1 shrink-0">
                {ability.costs.map((c, idx) => (
                  <CostChip
                    key={`cost-${idx}`}
                    cost={c}
                    satisfied={isCostSatisfied(c, resources)}
                    getResourceLabel={getResourceLabel}
                  />
                ))}
                {unmetReqs.map((req, idx) => (
                  <span
                    key={`req-${idx}`}
                    title={req.failReason}
                    className="flex items-center gap-1 text-[10px] px-1.5 py-0.5 rounded-[3px] whitespace-nowrap"
                    style={{ background: 'rgba(46, 23, 19, 0.65)', border: `1px solid ${garnet(0.5)}`, color: '#d8a39b' }}
                  >
                    <AlertTriangle className="size-3 shrink-0" strokeWidth={2} />
                    <span className="max-w-[120px] truncate">{req.failReason || 'requirement'}</span>
                  </span>
                ))}
              </span>
            )}
          </div>

          {/* 描述 —— RichText 解析 {keyword.id} 占位符出 hover tooltip。多行不 truncate。
              字体走 `--font-subtitle`(Cinzel Decorative 数字 + EB Garamond Latin + Noto Serif SC CJK
              的混合 stack,详见 styles/core/theme.css)。技能描述是"古书条目"质感的文本,跟系统 UI
              默认的 `--font-ui`(Segoe UI)调性不符,统一走 subtitle 让伤害公式 / 数字带 Cinzel 雕花感。 */}
          {ability.description && (
            <div
              className={[
                // `whitespace-pre-line` 保留 UE MultiLine 字段里设计师有意打的换行符,但多余空格仍正常折叠
                // (避免双空格 / 缩进意外被原样保留)。`break-words` 仍处理长 token 的内部断行。
                'text-[10.5px] whitespace-pre-line break-words transition-colors leading-snug mt-0.5',
                'font-[family-name:var(--font-subtitle)]',
                isActive && affordable ? 'text-parchment/95' : 'text-parchment-dark/80',
              ].join(' ')}
            >
              <RichText text={ability.description} keywords={BATTLE_KEYWORDS} styles={BATTLE_RICHTEXT_STYLES} />
            </div>
          )}
        </div>
      </div>
    </motion.li>
  );
}

/** Spatial-nav focus key —— 前缀避免跟其它页面撞车 */
function makeSkillFocusKey(abilityId: string): string {
  return `battle-skill-${abilityId}`;
}

/** Variant 行的 focus key,跟 ability 行用不同前缀避免被 spatial-nav 误匹配 */
function makeVariantFocusKey(abilityId: string, variantId: string): string {
  return `battle-variant-${abilityId}-${variantId}`;
}

export function BattleUnitPanel() {
  const selected = useBattleStore((s) => s.selectedUnit);
  const targeting = useBattleStore((s) => s.targeting);
  const [hoveredId, setHoveredId] = useState<string | null>(null);
  const [pressedId, setPressedId] = useState<string | null>(null);
  /**
   * 变体选择子模式:!= null 时,面板 body 切到该 ability 的变体列表,而不是技能列表。
   * 点 ability(有 variants)→ 设为 ability.id;点变体 / 按 Esc / 退出战斗 → 设为 null。
   *
   * 用 ability id 而非整个 ability 对象,这样 store push 更新让 ability 重新挂的同一 id
   * 自然 reconcile(picker 跟着新版本走,不指向 stale 引用)。
   */
  const [variantPickerForAbility, setVariantPickerForAbility] = useState<string | null>(null);
  const { unpinAll } = usePinnedTooltip();
  // getResourceLabel 不在这里取了 —— 渲染 resources 的逻辑挪进了 `<ResourcesSection>`,
  // 它内部自己 useResourceLabel。其它消费 label 的地方(SkillCard / VariantCard)也各自调 hook。

  // BattleUnitPanel 退场时 unpinAll —— 防止 panel 消失后 tooltip 仍悬浮。
  // variant picker 不在这里 reset:走下面的派生路径,raw `variantPickerForAbility` 可以保留
  // stale 值(memory cost = 一个 string ref),`showVariantPicker` 自然算成 false。
  useEffect(() => {
    if (selected === null) {
      unpinAll();
    }
  }, [selected, unpinAll]);

  // 派生 effective picker 状态 —— 把"何时强制关 picker"集中到这条派生,而不是分散到多个
  // useEffect + setState(被 react-hooks/set-state-in-effect 视为反 pattern 拒掉)。
  //
  // Picker 自然 disabled 的条件(任一为真就回到 ability 列表):
  //   - 没 selected unit / 进入瞄准期
  //   - 玩家没点过 variant ability(raw state == null)
  //   - 上次点的 ability 在新 selected 上不存在了(切角色、ability 移除)
  //   - 上次点的 ability 现在 variants 为空(designer 数据热更)
  //
  // 这样 raw `variantPickerForAbility` 只在 click handler 里被 set / unset(完全合规),
  // 跨 state 变化的 reset 逻辑全部在派生层做。
  const pickerAbility = (() => {
    if (!selected || targeting.active || !variantPickerForAbility) return null;
    const ability = selected.abilities.find((a) => a.id === variantPickerForAbility);
    if (!ability) return null;
    if ((ability.variants?.length ?? 0) === 0) return null;
    return ability;
  })();
  const showVariantPicker = pickerAbility !== null;

  // 手柄模式下 mount 完成后把 focus 落到第一个 row 上 —— 否则玩家第一次按 D-pad 找不到 anchor。
  // 在 variant picker 模式下 focus 第一个 variant,否则 focus 第一个 ability。
  const device = useUIInputStore((s) => s.device);
  const firstFocusKey = (() => {
    if (targeting.active) return undefined;
    if (showVariantPicker && pickerAbility) {
      const firstVariant = pickerAbility.variants?.[0];
      return firstVariant ? makeVariantFocusKey(pickerAbility.id, firstVariant.id) : undefined;
    }
    return selected?.abilities[0]?.id ? makeSkillFocusKey(selected.abilities[0].id) : undefined;
  })();
  useEffect(() => {
    if (!firstFocusKey) return;
    if (device !== 'gamepad') return;
    const handle = requestAnimationFrame(() => setFocus(firstFocusKey));
    return () => cancelAnimationFrame(handle);
  }, [firstFocusKey, device]);

  // 显式声明 UI navigation 输入接管权(BattleUnitPanel 是 always-mounted overlay,
  // stack-based IMC 激活对它无效,详见原注释)。
  const needsInput = Boolean(selected && !targeting.active && selected.abilities.length > 0);
  useEffect(() => {
    sendToUE('ui.input.setExplicitActive', { active: needsInput });
    return () => {
      sendToUE('ui.input.setExplicitActive', { active: false });
    };
  }, [needsInput]);

  const resources = selected?.resources ?? [];

  return (
    <AnimatePresence>
      {selected && (
        <motion.div
          // 进出场跟左侧 StatusPanel 镜像 —— 同节奏的优雅入场
          initial={{ opacity: 0, y: 10, scale: 0.98 }}
          animate={{ opacity: 1, y: 0, scale: 1 }}
          exit={{ opacity: 0, y: 10, scale: 0.98 }}
          transition={{ duration: 0.22, ease: [0.16, 1, 0.3, 1] }}
          className="absolute right-4 bottom-4 w-80 flex flex-col gap-2.5 p-3 select-none"
          // 共享卡面(双线规证书框)—— 跟 BattleUnitStatusPanel 同一份 BATTLE_PANEL_SURFACE
          style={BATTLE_PANEL_SURFACE}
        >
          {/* 内圈回声细线 + 四角饰 */}
          <PanelInnerRule />
          <CornerOrnaments size={14} offset={5} colorClass="text-brass/80" />

          {/* Body 切换:瞄准期 > 变体选择 > 技能列表(三态互斥,瞄准期优先级最高)。
              用 AnimatePresence 让 body 之间 crossfade,而非硬切 —— 玩家心智上"我推进了一层"
              的感觉跟着动画走更自然。mode="wait" 保证旧 body 先 exit 完才 enter 新的,
              避免两层叠加闪一帧。 */}
          <AnimatePresence mode="wait">
            {targeting.active ? (
              <motion.div
                key="targeting"
                initial={{ opacity: 0 }}
                animate={{ opacity: 1 }}
                exit={{ opacity: 0 }}
                transition={{ duration: 0.15 }}
              >
                <TargetingBody picked={targeting.picked} required={targeting.required} />
              </motion.div>
            ) : showVariantPicker && pickerAbility ? (
              <motion.div
                key={`variant-${pickerAbility.id}`}
                initial={{ opacity: 0, x: 10 }}
                animate={{ opacity: 1, x: 0 }}
                exit={{ opacity: 0, x: -10 }}
                transition={{ duration: 0.18, ease: [0.16, 1, 0.3, 1] }}
                className="flex flex-col gap-2.5"
              >
                <VariantPickerBody
                  ability={pickerAbility}
                  resources={resources}
                  hoveredId={hoveredId}
                  pressedId={pressedId}
                  onHoverChange={setHoveredId}
                  onPressStart={(id) => setPressedId(id)}
                  onPressEnd={(id) => setPressedId((prev) => (prev === id ? null : prev))}
                  onBack={() => setVariantPickerForAbility(null)}
                  onPickVariant={(variantId) => {
                    // 玩家选定变体 → 关 picker,把 abilityId+variantId 推给 C++ ExecuteAbility。
                    // 关 picker **先于** sendToUE,这样如果 C++ 处理慢 / 失败,UI 至少先回到了
                    // ability 列表,玩家不会卡在 picker 上。
                    setVariantPickerForAbility(null);
                    sendToUE('battleUI.abilityClicked', {
                      id: pickerAbility.id,
                      variantId,
                    });
                  }}
                />
              </motion.div>
            ) : (
              <motion.div
                key="ability-list"
                initial={{ opacity: 0 }}
                animate={{ opacity: 1 }}
                exit={{ opacity: 0 }}
                transition={{ duration: 0.15 }}
                className="flex flex-col gap-2.5"
              >
                {/* 资源条 */}
                <ResourcesSection resources={resources} />

                {/* 技能列表 */}
                <OrnateDivider>
                  <SectionHeader
                    label="Abilities"
                    icon={<ScrollText strokeWidth={1.8} />}
                    trailing={
                      selected.abilities.length > 0 && (
                        <span className="text-[9px] tabular-nums text-parchment-dark/50 px-1.5 py-0.5 rounded-full bg-panel-light">
                          {selected.abilities.length}
                        </span>
                      )
                    }
                  />
                </OrnateDivider>

                {selected.abilities.length === 0 ? (
                  <div className="text-xs text-parchment-dark/50 italic text-center py-2">
                    无可用技能
                  </div>
                ) : (
                  <ul className="flex flex-col gap-1.5">
                    {selected.abilities.map((a) => {
                      const hasVariants = (a.variants?.length ?? 0) > 0;
                      return (
                        <SkillCard
                          key={a.id}
                          ability={a}
                          focusKey={makeSkillFocusKey(a.id)}
                          isHovered={hoveredId === a.id}
                          isPressed={pressedId === a.id}
                          affordable={isAbilityAffordable(a.costs, a.requirements, resources)}
                          resources={resources}
                          onHoverChange={(id) => setHoveredId(id)}
                          onPressStart={() => setPressedId(a.id)}
                          onPressEnd={() => setPressedId((prev) => (prev === a.id ? null : prev))}
                          onActivate={() => {
                            // 有变体 → 进 picker,**不** ExecuteAbility(变体选完才执行)
                            // 无变体 → 老路径,直接发 abilityClicked
                            if (hasVariants) {
                              setVariantPickerForAbility(a.id);
                            } else {
                              sendToUE('battleUI.abilityClicked', { id: a.id });
                            }
                          }}
                        />
                      );
                    })}
                  </ul>
                )}
              </motion.div>
            )}
          </AnimatePresence>
        </motion.div>
      )}
    </AnimatePresence>
  );
}

// ─── Resources Section ──────────────────────────────────────────
//
// 资源条小组件 —— ability 列表 body 跟 variant picker body 共用。抽出来既避免 JSX
// 重复,也保证两个 body 视觉完全一致(同款 OrnateDivider + 同款 ResourcePill 网格)。
//
// `resources` 为空数组时整块不渲染 —— 父逻辑(`<ResourcesSection>` 的 caller)不用再判断。

function ResourcesSection({ resources }: { resources: BattleResource[] }) {
  const getResourceLabel = useResourceLabel();
  if (resources.length === 0) return null;
  return (
    <>
      <OrnateDivider>
        <SectionHeader label="Resources" icon={<BookOpen strokeWidth={1.8} />} />
      </OrnateDivider>
      <div className="flex flex-wrap gap-1.5">
        {resources.map((res) => (
          <ResourcePill
            key={res.id}
            resource={res}
            label={getResourceLabel(res.id)}
          />
        ))}
      </div>
    </>
  );
}

// ─── Variant Picker Body ─────────────────────────────────────────
//
// 玩家点了带变体的 ability 后,面板 body 切到这里:小 back 按钮 + 父 ability 名 +
// variant 列表。点 variant → 关 picker + 发 abilityClicked。Esc/back → 关 picker 回到 ability 列表。
//
// 视觉上跟 ability 列表用同款 SkillCard 模式 —— variant 也是完整 ability(heavy variant
// 模型),自带 cost / requirement,affordability 算法跟 SkillCard 完全共享。

interface VariantPickerBodyProps {
  ability: Ability;
  resources: BattleResource[];
  hoveredId: string | null;
  pressedId: string | null;
  onHoverChange: (id: string | null) => void;
  onPressStart: (id: string) => void;
  onPressEnd: (id: string) => void;
  onBack: () => void;
  onPickVariant: (variantId: string) => void;
}

function VariantPickerBody({
  ability,
  resources,
  hoveredId,
  pressedId,
  onHoverChange,
  onPressStart,
  onPressEnd,
  onBack,
  onPickVariant,
}: VariantPickerBodyProps) {
  const variants = ability.variants ?? [];
  return (
    <>
      {/* Header:back button + 父 ability 名 + "Esc 返回"提示 */}
      <div className="flex items-center gap-2 pb-1">
        <button
          type="button"
          onClick={onBack}
          className="flex items-center gap-1 text-[10px] uppercase tracking-wider text-parchment-dark hover:text-gold-light transition-colors"
          // native button 在 Ultralight 下 hover/click 都正常,不需要做成 motion.div
        >
          <ArrowLeft className="size-3" strokeWidth={2.5} />
          <span>Back</span>
        </button>
        <span className="flex-1 flex items-center justify-center gap-1.5 min-w-0 text-xs font-[family-name:var(--font-display)] text-gold-light">
          {/* 父技能纹章 + 名字 —— 提示玩家"当前在哪个技能的变体页"。 */}
          <AbilityIcon name={ability.icon} className="size-4 shrink-0" />
          <span className="truncate">
            {/* 父 ability 名同样走 RichText —— 步枪名带混合字体的话 picker header 也要跟 SkillCard 里一致。 */}
            <RichText text={ability.displayName} keywords={BATTLE_KEYWORDS} styles={BATTLE_RICHTEXT_STYLES} />
          </span>
        </span>
        {/* 占位,让中间标题居中(跟左侧 Back 视觉等宽) */}
        <span className="w-12" aria-hidden />
      </div>

      {/* 资源条 —— 变体的 cost 不同,玩家需要看到当前持有量才能做出选择决策。
          跟 ability 列表 body 完全同款渲染。 */}
      <ResourcesSection resources={resources} />

      <OrnateDivider>
        <SectionHeader
          label="Variants"
          icon={<Sparkles strokeWidth={1.8} />}
          trailing={
            variants.length > 0 && (
              <span className="text-[9px] tabular-nums text-parchment-dark/50 px-1.5 py-0.5 rounded-full bg-panel-light">
                {variants.length}
              </span>
            )
          }
        />
      </OrnateDivider>

      {variants.length === 0 ? (
        // 理论上走不到 —— 父逻辑判断 variants.length > 0 才进 picker。兜底显示。
        <div className="text-xs text-parchment-dark/50 italic text-center py-2">
          无可用变体
        </div>
      ) : (
        <ul className="flex flex-col gap-1.5">
          {variants.map((v) => (
            <VariantCard
              key={v.id}
              variant={v}
              parentIcon={ability.icon}
              focusKey={makeVariantFocusKey(ability.id, v.id)}
              isHovered={hoveredId === v.id}
              isPressed={pressedId === v.id}
              // Variant 的 affordability 跟 SkillCard 同款算 —— variant 自己的 costs +
              // requirements 跟 unit 当前 resources 对照。不够花就置灰,玩家选择前一眼看到。
              affordable={isAbilityAffordable(v.costs, v.requirements, resources)}
              resources={resources}
              onHoverChange={onHoverChange}
              onPressStart={() => onPressStart(v.id)}
              onPressEnd={() => onPressEnd(v.id)}
              onActivate={() => onPickVariant(v.id)}
            />
          ))}
        </ul>
      )}
    </>
  );
}

interface VariantCardProps {
  variant: BattleAbilityVariant;
  /** 父技能的图标 key —— variant 自己没配 icon 时的第一级回退(同族变体默认沿用父纹章)。 */
  parentIcon?: string;
  focusKey: string;
  isHovered: boolean;
  isPressed: boolean;
  affordable: boolean;
  /** 同 SkillCard.resources —— cost chip 逐条标红/标金需要资源池上下文。 */
  resources: BattleResource[];
  onHoverChange: (id: string | null) => void;
  onPressStart: () => void;
  onPressEnd: () => void;
  onActivate: () => void;
}

function VariantCard({
  variant,
  parentIcon,
  focusKey,
  isHovered,
  isPressed,
  affordable,
  resources,
  onHoverChange,
  onPressStart,
  onPressEnd,
  onActivate,
}: VariantCardProps) {
  const getResourceLabel = useResourceLabel();
  const { ref, focused } = useFocusable({
    focusKey,
    onEnterPress: () => {
      // 跟 SkillCard 同款:不够花的 variant 允许 focus(看 cost chip),但 Enter 不触发
      if (!affordable) return;
      onActivate();
    },
    onFocus: () => onHoverChange(variant.id),
  });
  const isActive = isHovered || focused;
  const unmetReqs = variant.requirements.filter((r) => !r.met);

  return (
    <motion.li
      ref={ref}
      onMouseEnter={() => onHoverChange(variant.id)}
      onMouseLeave={() => {
        onHoverChange(null);
        onPressEnd();
      }}
      onMouseDown={() => { if (affordable) onPressStart(); }}
      onMouseUp={onPressEnd}
      onClick={() => { if (affordable) onActivate(); }}
      animate={{
        x: isActive && affordable ? 2 : 0,
        scale: isPressed && affordable ? 0.98 : 1,
      }}
      transition={{ duration: 0.12, ease: 'easeOut' }}
      className={[
        'relative px-2.5 py-1.5 select-none transition-colors duration-150',
        // 同 SkillCard 注释:**不要**用 grayscale filter,只用 opacity。详见 SkillCard 同款条件那里。
        affordable ? 'cursor-pointer' : 'cursor-not-allowed opacity-50',
      ].join(' ')}
      // 跟 SkillCard 同一份三态卡面(cardSurface)—— variant 也是完整 ability,视觉无差别
      style={cardSurface(isActive, isPressed, affordable)}
    >
      {/* 跟 SkillCard 同款的 active chevron */}
      <motion.div
        className="absolute left-0.5 top-1/2 -translate-y-1/2 text-gold-light"
        animate={{ opacity: isActive && affordable ? 1 : 0, x: isActive && affordable ? 0 : -4 }}
        transition={{ duration: 0.12 }}
        aria-hidden
      >
        <ChevronRight className="size-3" strokeWidth={2.5} />
      </motion.div>

      {/* 图标 + 内容双列 —— 跟 SkillCard 同款布局。variant 没配 icon 时回退父技能图标
          (同族变体沿用父纹章),再没有才落默认纹章。 */}
      <div className="flex gap-2 pl-3">
        <AbilityIcon
          name={variant.icon}
          fallbackName={parentIcon}
          className={[
            'size-6 shrink-0 mt-0.5 transition-colors',
            // 常态全量 gold(不再 /75 压暗)—— 剪影图标大面积实心,亮度低一档整列会发闷;
            // active 仍用更亮的 gold-light 保持三态区分
            isActive && affordable ? 'text-gold-light' : affordable ? 'text-gold' : 'text-parchment-dark',
          ].join(' ')}
        />

        <div className="flex-1 min-w-0">
          <div className="flex items-center justify-between gap-2">
            <span className={[
              'text-sm font-[family-name:var(--font-display)] transition-colors truncate',
              isActive && affordable ? 'text-gold-light' : affordable ? 'text-parchment' : 'text-parchment-dark',
            ].join(' ')}>
              {/* Variant 名走 RichText,同 SkillCard 标题:支持 victorian 混合字体 token。 */}
              <RichText text={variant.displayName} keywords={BATTLE_KEYWORDS} styles={BATTLE_RICHTEXT_STYLES} />
            </span>

            {(variant.costs.length > 0 || unmetReqs.length > 0) && (
              <span className="flex items-center gap-1 shrink-0">
                {variant.costs.map((c, idx) => (
                  <CostChip
                    key={`cost-${idx}`}
                    cost={c}
                    satisfied={isCostSatisfied(c, resources)}
                    getResourceLabel={getResourceLabel}
                  />
                ))}
                {unmetReqs.map((req, idx) => (
                  <span
                    key={`req-${idx}`}
                    title={req.failReason}
                    className="flex items-center gap-1 text-[10px] px-1.5 py-0.5 rounded-[3px] whitespace-nowrap"
                    style={{ background: 'rgba(46, 23, 19, 0.65)', border: `1px solid ${garnet(0.5)}`, color: '#d8a39b' }}
                  >
                    <AlertTriangle className="size-3 shrink-0" strokeWidth={2} />
                    <span className="max-w-[120px] truncate">{req.failReason || 'requirement'}</span>
                  </span>
                ))}
              </span>
            )}
          </div>

          {variant.description && (
            <div className={[
              // `whitespace-pre-line` 保留 UE MultiLine 字段里设计师有意打的换行符,但多余空格仍正常折叠
              // (避免双空格 / 缩进意外被原样保留)。`break-words` 仍处理长 token 的内部断行。
              'text-[10.5px] whitespace-pre-line break-words transition-colors leading-snug mt-0.5',
              'font-[family-name:var(--font-subtitle)]',  // 跟 SkillCard 描述同字体(详见那边注释)
              isActive && affordable ? 'text-parchment/95' : 'text-parchment-dark/80',
            ].join(' ')}>
              {/* Variant 描述跟 SkillCard 同款走 RichText —— 支持 `{keyword.id}` 链接 + `{style.*:*}`
                  内联样式。step weapon 变体描述里可能要混排 caliber / brand 等装饰段。 */}
              <RichText text={variant.description} keywords={BATTLE_KEYWORDS} styles={BATTLE_RICHTEXT_STYLES} />
            </div>
          )}
        </div>
      </div>
    </motion.li>
  );
}

// ─── Targeting Body ──────────────────────────────────────────────
//
// 瞄准期面板内容:大号 Crosshair icon + "目标 X/Y" 大字 + 确认按钮 + cancel hint。
// 玩家用鼠标点世界中的目标,picked < required 时也允许"确认"按钮强行提交
// (用例:AOE 要 3 个,场上只剩 1 个;玩家点确认按当前 1 个目标提交)。

function TargetingBody({ picked, required }: { picked: number; required: number }) {
  const isComplete = picked >= required;
  // 确认按钮可用条件:至少有 1 个 picked target。C++ 端也会再做一次 gate,这里是 UX 上提前 disable
  // 避免无效点击。`required === 0`(无限制)同样要求至少 1 个 —— 0 目标 commit 无意义,
  // 强 commit 等价于 cancel。
  const canConfirm = picked > 0;
  // 自然完成(picked === required > 0)状态下,确认按钮**不显示** —— state tree 走自然 commit 路径,
  // 玩家此时通常已经在点最后一个目标的同一动作里,弹个按钮反而是干扰。确认按钮**只在**"还差几个但
  // 玩家想现在就停"的偏门场景出现。required===1 时永远不会显示确认按钮(picked==0 或 picked==1=完成)。
  const showConfirm = canConfirm && !isComplete;

  const [isConfirmHovered, setConfirmHovered] = useState(false);
  const [isConfirmPressed, setConfirmPressed] = useState(false);

  return (
    <div className="flex flex-col items-center gap-2 py-4">
      {/* Crosshair icon —— 持续旋转作为 "active" 提示。完成时停止,色变月桂绿。
          drop-shadow 光晕已移除(版画语言),旋转本身已是足够的 active 信号 */}
      <motion.div
        animate={{ rotate: isComplete ? 0 : 360 }}
        transition={{ duration: 8, ease: 'linear', repeat: isComplete ? 0 : Infinity }}
        className="relative"
      >
        <Crosshair
          className="size-10 transition-colors duration-300"
          strokeWidth={1.4}
          style={{ color: isComplete ? '#9ec9a4' : '#f0d080' }}
        />
        {/* 内部脉冲圆点 —— 实心,无 glow */}
        <motion.div
          className="absolute inset-0 flex items-center justify-center"
          animate={{ scale: [1, 1.6, 1], opacity: [0.8, 0, 0.8] }}
          transition={{ duration: 1.6, repeat: Infinity, ease: 'easeOut' }}
        >
          <span className="size-1.5 rounded-full bg-gold-light" />
        </motion.div>
      </motion.div>

      <div className="text-[10px] uppercase tracking-[0.3em] text-parchment-dark/70">
        选取目标
      </div>

      {/* 大号 X/Y —— subtitle 字体显得有仪式感。完成色用月桂绿 ink(battle-palette 同源) */}
      <div className="font-[family-name:var(--font-subtitle)] text-3xl tabular-nums leading-none flex items-baseline gap-1">
        <motion.span
          // picked 增加时 spring-pop
          key={picked}
          initial={{ scale: 0.6, opacity: 0 }}
          animate={{ scale: 1, opacity: 1 }}
          transition={{ type: 'spring', stiffness: 500, damping: 18 }}
          style={{ color: isComplete ? '#9ec9a4' : '#f0d080', textShadow: '0 1px 3px rgba(0,0,0,0.6)' }}
        >
          {picked}
        </motion.span>
        <span className="text-parchment-dark/60 text-2xl">/</span>
        <span className="text-parchment-dark text-2xl">{required}</span>
      </div>

      {/* 确认按钮 —— 只在 "已选 ≥ 1 但还没集满" 时出现。AnimatePresence 包让进出有 fade 衔接。
          点击发 targetingConfirmed event,C++ AbilityController 转发 Intent.TargetingConfirmed,
          state tree 的"等够目标"task 收到后跳出"等满"路径,走 commit 流程。 */}
      <AnimatePresence>
        {showConfirm && (
          <motion.button
            key="confirm-button"
            type="button"
            initial={{ opacity: 0, y: 4 }}
            animate={{ opacity: 1, y: 0 }}
            exit={{ opacity: 0, y: 4 }}
            transition={{ duration: 0.18 }}
            onMouseEnter={() => setConfirmHovered(true)}
            onMouseLeave={() => { setConfirmHovered(false); setConfirmPressed(false); }}
            onMouseDown={() => setConfirmPressed(true)}
            onMouseUp={() => setConfirmPressed(false)}
            onClick={() => sendToUE('battleUI.targetingConfirmed', {})}
            className="mt-1 select-none"
            // 平面雕版按钮:不透明月桂墨绿底(三档递进)+ 细线框 + inset 回声细线
            // (双线规,呼应面板外框)。粗 2px 边 / 外投影 / 内嵌斜面都已移除。
            style={{
              padding: '0.4em 1.4em',
              fontSize: '0.8em',
              fontWeight: 600,
              letterSpacing: '0.14em',
              fontFamily: 'var(--font-display)',
              background: isConfirmPressed ? '#243a2c' : isConfirmHovered ? '#37533f' : '#2c4334',
              color: isConfirmHovered || isConfirmPressed ? '#eef7e8' : '#cfe0c6',
              border: `1px solid ${isConfirmHovered || isConfirmPressed ? 'rgba(157, 201, 164, 0.85)' : 'rgba(125, 168, 131, 0.55)'}`,
              borderRadius: 3,
              cursor: 'pointer',
              boxShadow: 'inset 0 0 0 1px rgba(125, 168, 131, 0.22)',
              transition: 'background 120ms ease, border-color 120ms ease, color 120ms ease',
              textShadow: '0 1px 1px rgba(0,0,0,0.5)',
            }}
          >
            确认
          </motion.button>
        )}
      </AnimatePresence>

      <div className="flex items-center gap-1.5 text-[10px] text-parchment-dark/60 italic mt-1">
        <kbd className="px-1.5 py-0.5 rounded bg-panel-light border border-panel-border text-parchment-dark not-italic font-mono text-[9px]">
          Esc
        </kbd>
        <span>取消</span>
      </div>
    </div>
  );
}
