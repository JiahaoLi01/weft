import { createElement, useMemo } from 'react';
import { AnimatePresence, motion } from 'framer-motion';
import { ShieldUser, Activity, Sparkles, EyeOff } from 'lucide-react';
import { useBattleStore, type BattleAttribute, type BattleBuff, type BattleBuffModifier } from '@/stores/battle-store';
import { RichText, type BuffTooltipModifier } from '@/components/NestedTooltip';
import { useAttributeLabel } from '@/i18n/battle-labels';
import { BATTLE_KEYWORDS } from '@/config/battle-keywords';
import { BATTLE_RICHTEXT_STYLES } from '@/config/battle-richtext-styles';
import { BATTLE_PANEL_SURFACE, CornerOrnaments, OrnateDivider, PanelInnerRule, SectionHeader } from '@/components/battle/PanelDecorations';
import { HpBar } from '@/components/battle/HpBar';
import { SmokeBg, type SmokeParams } from '@/components/SmokeBg';
import { getAttributeIcon } from '@/components/battle/battle-icons';
import { ANTIQUE_GOLD, brass, garnet, laurel, TONE_INK } from '@/config/battle-palette';

// MaxHealth attribute 全名 —— 跟 C++ `FGameplayAttributeField::FromProperty` 的格式
// "DeclaringClassName.PropertyName" 对齐。改 C++ 端的 attribute class 名 / property 名时
// 要同步这里。
const MAX_HEALTH_ATTRIBUTE_ID = 'CRPGGameplayAttributeData.MaxHealth';

/**
 * BattleUnitStatusPanel —— 屏幕左下角的"选中单位状态面板"。
 *
 * 视觉构成(从上到下):
 *   1. 头部 banner:角色名 + 头像槽(临时用 ShieldUser icon 占位 —— 后期换成 portrait)
 *   2. HP 条:带 Heart icon + 渐变填充 + shimmer + 濒危脉冲
 *   3. 属性面板:列出每条 attribute,带 icon + base/current 对比
 *   4. 状态(Buff):**展开式条目列表** —— 名称 / 剩余回合 / 描述 / 属性修改全部内联,
 *      超高走滚动(battle-buff-list 黄铜细滚动条)。早期版本是 chip + hover tooltip,
 *      玩家反馈"悬浮看描述太慢",改为直接展开,tooltip 路径随之移除。
 *
 * 跟 BattleUnitPanel(右下角)是配对的两个 panel:
 *   - 右下:技能 + 资源(操作面)
 *   - 左下:身份信息 + 状态(状态面)
 *
 * 跟 BattleUnitPanel 一样用 AnimatePresence 跟 selected 同步进出场。
 */

// ── Helpers ──────────────────────────────────────────────────────

/**
 * 数字格式化辅助:整数不带小数点;非整数保留两位。负数用真正的 "−"(U+2212)
 * 不是 ASCII "-",tabular-nums 排版下宽度一致。
 */
function formatNumber(n: number): string {
  const abs = Math.abs(n);
  return Number.isInteger(abs) ? abs.toString() : abs.toFixed(2);
}

/**
 * 属性修改器格式化 —— 输出 displayValue + tone(正绿 / 负红 / 中性默认色)。
 * Multi 走百分比 delta,FinalMulti 是真乘数,语义不同(见 CRPGGameplayAttribute.cpp)。
 */
function formatModifier(mod: BattleBuffModifier): { text: string; tone: BuffTooltipModifier['tone'] } {
  const valStr = formatNumber(mod.value);
  switch (mod.op) {
    case 'Add': {
      if (mod.value > 0) return { text: `+${valStr}`, tone: 'positive' };
      if (mod.value < 0) return { text: `−${valStr}`, tone: 'negative' };
      return { text: '0', tone: 'neutral' };
    }
    case 'Multi': {
      const pctStr = formatNumber(mod.value * 100);
      if (mod.value > 0) return { text: `+${pctStr}%`, tone: 'positive' };
      if (mod.value < 0) return { text: `−${pctStr}%`, tone: 'negative' };
      return { text: '+0%', tone: 'neutral' };
    }
    case 'FinalAdd': {
      if (mod.value > 0) return { text: `+${valStr} (final)`, tone: 'positive' };
      if (mod.value < 0) return { text: `−${valStr} (final)`, tone: 'negative' };
      return { text: '0 (final)', tone: 'neutral' };
    }
    case 'FinalMulti': {
      if (mod.value > 1) return { text: `×${valStr}`, tone: 'positive' };
      if (mod.value < 1) return { text: `×${valStr}`, tone: 'negative' };
      return { text: '×1', tone: 'neutral' };
    }
    case 'ForceSet':
      return { text: `= ${valStr}`, tone: 'neutral' };
    default:
      return { text: valStr, tone: 'neutral' };
  }
}

/**
 * 判断一条 buff 整体是"增益 / 削减 / 中性"—— 看 modifiers 多数 tone。这是为了在 chip
 * 上画一个 tone-aware 的小圆点,玩家不用 hover 也能区分。
 *
 * 规则:有任一 positive → positive;有 negative 但没 positive → negative;都没有 / 平衡 → neutral。
 * 简单启发式,够用。复杂 buff(混 +攻击 -防御)显示成 positive,设计师可以靠 buff 名称澄清。
 */
function inferBuffTone(modifiers: BuffTooltipModifier[]): 'positive' | 'negative' | 'neutral' {
  let hasPos = false;
  let hasNeg = false;
  for (const m of modifiers) {
    if (m.tone === 'positive') hasPos = true;
    if (m.tone === 'negative') hasNeg = true;
  }
  if (hasPos) return 'positive';
  if (hasNeg) return 'negative';
  return 'neutral';
}

/** Buff 展开条目的视图模型 —— 直接内联展示,不再经 KeywordDef / hover tooltip。 */
interface BuffEntryVM {
  id: string;
  name: string;
  description: string;
  lifetimeType: BattleBuff['lifetimeType'];
  /** 剩余回合数(单位契约:回合,原值直推,见 battle-store BattleBuff)。 */
  remainingRounds: number;
  modifiers: BuffTooltipModifier[];
  /** 'special' 只来自显式 Meta.Special(推断路径永远只产出前三种)。 */
  tone: 'positive' | 'negative' | 'neutral' | 'special';
  /** 稀有度(Meta.Rarity.*)。special 烟雾按此配色,未标落回古金。 */
  rarity?: BattleBuff['rarity'];
  /** 仅开发期:被强制显示的隐藏 buff(Meta.Hidden),UI 打标提示。 */
  hidden?: boolean;
}

function buffToEntry(buff: BattleBuff, getAttrLabel: (raw: string) => string): BuffEntryVM {
  // displayName 由 C++ FBuffOverrideGroup::GetDisplayText 生成,Stack 模式自带 "× N",
  // 这里不再单独渲染层数徽章。
  const modifiers: BuffTooltipModifier[] = buff.modifiers.map((mod) => {
    const { text, tone } = formatModifier(mod);
    return {
      attributeLabel: getAttrLabel(mod.attribute),
      displayValue: text,
      tone,
    };
  });
  return {
    id: buff.id,
    name: buff.displayName,
    description: buff.description,
    lifetimeType: buff.lifetimeType,
    remainingRounds: buff.remainingRounds,
    modifiers,
    // 显式极性(Meta.Positive/Negative)优先;没标才按属性增减推断。
    tone: buff.polarity ?? inferBuffTone(modifiers),
    rarity: buff.rarity,
    hidden: buff.hidden,
  };
}

/**
 * 剩余时长标签。Limited → "剩余 N 回合"(N 已在 C++ 端 eps-ceil 取整,这里原样显示,
 * 不做任何浮点处理);Infinity → "永久";其它不显示。
 */
function formatRemaining(e: BuffEntryVM): string {
  if (e.lifetimeType === 'Infinity') {
    return '永久';
  }
  if (e.lifetimeType !== 'Limited') {
    return '';
  }
  if (e.remainingRounds >= 0) {
    return `剩余 ${e.remainingRounds} 回合`;
  }
  return '';
}

/**
 * special buff 的背景特效 —— 从右缘向左飘散的烟雾(SmokeBg,canvas2D @15fps)。
 *
 * 曾经是三层错位描边的 SVG 浮雕(39 个 stroked 图元),在 Ultralight 的整 View 重绘下
 * 每帧代价过高(实测卡顿),换成 nested tooltip 同款的烟雾管线:96×24 低分辨率画布
 * CPU 光栅 + CSS 拉伸,IntersectionObserver 离屏自动停算。rotate90 让竖版云纹躺倒、
 * 扩散顺水平轴;fade='right' 右浓左淡,文字区(左侧)保持干净可读。
 *
 * 烟色按稀有度(Meta.Rarity.*,同 nested tooltip 的 rarity 色):common 灰 / rare 蓝 /
 * epic 紫 / legendary 琥珀;未标稀有度落回 'special' 古金。
 */

/** 用户在 SmokeBuffLab(pushUI 'smoke-buff-lab')调定的参数,2026-07-15。要改再进实验室调。
 *  导出:Pick17 构筑页的稀有度卡复用同一份烟雾参数(「稀有度样式 = buff 的烟雾特效」)。 */
export const SPECIAL_SMOKE_PARAMS: SmokeParams = {
  cloudScale: 7.2,
  noiseScale: 4,
  cloudSpeedX: 2,
  cloudSpeedY: 3,
  noiseSpeedX: -1.5,
  noiseSpeedY: -2,
  gamma: 0.3,
  noiseStrength: 0.3,
  fadePower: 2.5,
};

function SpecialBuffSmoke({ rarity }: { rarity?: BattleBuff['rarity'] }) {
  return (
    <SmokeBg
      rarity={rarity ?? 'special'}
      params={SPECIAL_SMOKE_PARAMS}
      fade="right"
      rotate90
      resolution={{ w: 96, h: 24 }}
      className="absolute inset-0 w-full h-full pointer-events-none"
    />
  );
}

/**
 * Chip 颜色 map —— tone 决定 chip 细线描边 / 平面底色 + 左侧 dot 色。
 * 油画三色(月桂绿 / 石榴红 / 黄铜),dot 是实心小圆,无 glow。
 *
 * special 不使用第四种色相,回到面板原有的深褐、黄铜与羊皮纸原色。它与 neutral 的
 * 区别 = 钢印内阴影(boxShadow)+ 从右向左飘散的古金烟雾背景(SpecialBuffSmoke)。
 */
const TONE_CHIP_STYLES: Record<
  'positive' | 'negative' | 'neutral' | 'special',
  { border: string; background: string; dot: string; boxShadow?: string; dotShadow?: string }
> = {
  positive: { border: `1px solid ${laurel(0.5)}`, background: 'rgba(31, 40, 30, 0.6)', dot: '#7da883' },
  negative: { border: `1px solid ${garnet(0.5)}`, background: 'rgba(44, 25, 20, 0.6)', dot: '#b05c52' },
  neutral:  { border: `1px solid ${brass(0.4)}`,  background: 'rgba(40, 31, 19, 0.6)', dot: ANTIQUE_GOLD },
  special:  {
    border: `1px solid ${brass(0.58)}`,
    background: 'rgba(39, 31, 20, 0.76)',
    dot: '#c4ad78',
    boxShadow: [
      'inset 0 1px 0 rgba(239, 220, 174, 0.15)',
      'inset 0 -1px 0 rgba(0, 0, 0, 0.72)',
      'inset 1px 0 0 rgba(239, 220, 174, 0.07)',
      'inset -1px 0 0 rgba(0, 0, 0, 0.45)',
      '0 1px 0 rgba(0, 0, 0, 0.28)',
    ].join(', '),
    dotShadow: [
      '-0.5px -0.5px 0 rgba(239, 220, 174, 0.42)',
      '0.5px 0.5px 0 rgba(0, 0, 0, 0.8)',
      'inset 0 0 0 1px rgba(82, 61, 29, 0.75)',
    ].join(', '),
  },
};

// ── Component ────────────────────────────────────────────────────

export function BattleUnitStatusPanel() {
  const selected = useBattleStore((s) => s.selectedUnit);
  const getAttrLabel = useAttributeLabel();

  // 把所有 buff 翻译成展开条目 VM。useMemo 避免每帧重建对象数组 → 子项 motion 抖动。
  const buffEntries = useMemo<BuffEntryVM[]>(() => {
    if (!selected) return [];
    return selected.buffs.map((b) => buffToEntry(b, getAttrLabel));
  }, [selected, getAttrLabel]);

  // HP 数据:current 来自 BEAC.Health 直推,max 从 attributes 表 lookup MaxHealth。
  const maxHealthAttr: BattleAttribute | undefined = useMemo(
    () => selected?.attributes.find((a) => a.id === MAX_HEALTH_ATTRIBUTE_ID),
    [selected],
  );
  const currentHp = selected?.currentHealth ?? 0;
  const maxHp = maxHealthAttr?.current ?? 0;

  // 非 HP 的其它属性 —— 在属性面板里列出。MaxHealth 已经在 HP bar 里展示了,这里跳过避免冗余。
  const otherAttributes = useMemo(
    () => (selected?.attributes ?? []).filter((a) => a.id !== MAX_HEALTH_ATTRIBUTE_ID),
    [selected],
  );

  return (
    <AnimatePresence>
      {selected && (
        <motion.div
          // 进出场跟 BattleUnitPanel 镜像 —— y 偏移 +6,从下方滑入,同节奏方便玩家把两个 panel 视觉上当一组看
          initial={{ opacity: 0, y: 10, scale: 0.98 }}
          animate={{ opacity: 1, y: 0, scale: 1 }}
          exit={{ opacity: 0, y: 10, scale: 0.98 }}
          transition={{ duration: 0.22, ease: [0.16, 1, 0.3, 1] }}  // expo-out 曲线,优雅入场
          className="absolute left-4 bottom-4 w-80 flex flex-col gap-2.5 p-3 select-none"
          // 共享卡面(双线规证书框)—— 跟 BattleUnitPanel 同一份 BATTLE_PANEL_SURFACE
          style={BATTLE_PANEL_SURFACE}
        >
          {/* 内圈回声细线 + 四角饰 —— 装饰层,不截 click */}
          <PanelInnerRule />
          <CornerOrnaments size={14} offset={5} colorClass="text-brass/80" />

          {/* ── Header:头像槽 + 角色名 ──────────────────────── */}
          <div className="relative flex items-center gap-2.5 pb-2 mt-0.5">
            {/* 头像槽 —— 椭圆 cameo 浮雕框(维多利亚胸像剪影的承载形):黄铜外圈 +
                暗色间隙 + 内圈细线,全部 0 blur 的 inset ring,平面。临时用 ShieldUser
                icon 占位,后期换成 portrait texture(圆形裁切后正好嵌进 cameo)。 */}
            <div
              className="size-11 rounded-full flex items-center justify-center shrink-0"
              style={{
                background: '#221a10',
                border: `1px solid ${brass(0.6)}`,
                boxShadow: `inset 0 0 0 2px rgba(20,15,10,0.9), inset 0 0 0 3px ${brass(0.28)}`,
              }}
            >
              <ShieldUser className="size-6 text-brass" strokeWidth={1.5} />
            </div>

            {/* 角色名 + subtitle 装饰 */}
            <div className="flex flex-col flex-1 min-w-0">
              <span className="text-[10px] tracking-[0.3em] uppercase text-parchment-dark/60 leading-none mb-0.5">
                Selected
              </span>
              <span
                className="font-[family-name:var(--font-display)] text-parchment text-base font-semibold truncate leading-tight"
                // 只留 1px 落影保证可读性 —— 名字是"铭刻"不是"霓虹灯牌",金色光晕已移除
                style={{ textShadow: '0 1px 2px rgba(0,0,0,0.6)', letterSpacing: '0.04em' }}
                title={selected.name}
              >
                {selected.name}
              </span>
            </div>
          </div>

          {/* 头部 ornate divider */}
          <OrnateDivider />

          {/* ── HP Bar ─────────────────────────────────────── */}
          <HpBar current={currentHp} max={maxHp} />

          {/* ── 属性面板 ──────────────────────────────────── */}
          {otherAttributes.length > 0 && (
            <>
              <OrnateDivider>
                <SectionHeader label="Attributes" icon={<Activity strokeWidth={1.8} />} />
              </OrnateDivider>
              <ul className="flex flex-col gap-1">
                {otherAttributes.map((attr) => {
                  const changed = attr.base !== attr.current;
                  const buffed = attr.current > attr.base;
                  // 同 BattleUnitPanel 注释:createElement 走 dynamic icon,绕开
                  // react-hooks/static-components 的语法启发式 false-positive。
                  const iconEl = createElement(getAttributeIcon(attr.id), {
                    className: 'size-3.5 text-gold/60',
                    strokeWidth: 1.8,
                  });
                  return (
                    <li
                      key={attr.id}
                      className="flex items-center justify-between text-[11px] px-1.5 py-1 rounded transition-colors hover:bg-[rgba(199,166,103,0.06)]"
                    >
                      <span className="flex items-center gap-1.5 text-parchment-dark">
                        {iconEl}
                        {/* 属性名走 subtitle 混合栈(中文 LXGW 宋体)—— 跟同行数值 / buff 描述
                            统一。早期是裸 span,继承 body 的 --font-ui(Segoe UI 无衬线),
                            在一票衬线文本里显得突兀。 */}
                        <span className="font-[family-name:var(--font-subtitle)]">{getAttrLabel(attr.id)}</span>
                      </span>
                      <span className="tabular-nums font-[family-name:var(--font-subtitle)]">
                        {/* 增减值走油画 tone ink(battle-palette),不用 emerald/rose 霓虹档 */}
                        <span
                          className={changed ? 'font-semibold' : 'text-parchment'}
                          style={changed ? { color: buffed ? TONE_INK.positive : TONE_INK.negative } : undefined}
                        >
                          {formatNumber(attr.current)}
                        </span>
                        {changed && (
                          <span className="text-parchment-dark/60 ml-1 text-[10px]">
                            ({formatNumber(attr.base)})
                          </span>
                        )}
                      </span>
                    </li>
                  );
                })}
              </ul>
            </>
          )}

          {/* ── 状态(Buff)─────────────────────────────────── */}
          <OrnateDivider>
            <SectionHeader
              label="Status"
              icon={<Sparkles strokeWidth={1.8} />}
              trailing={
                buffEntries.length > 0 && (
                  <span className="text-[9px] tabular-nums text-parchment-dark/50 px-1.5 py-0.5 rounded-full bg-panel-light">
                    {buffEntries.length}
                  </span>
                )
              }
            />
          </OrnateDivider>
          {buffEntries.length === 0 ? (
            <div className="text-[10px] text-parchment-dark/50 italic text-center py-1">
              无状态效果
            </div>
          ) : (
            // 展开列表 —— 名称 / 剩余回合 / 描述 / 属性修改全部内联可读,
            // 超高走滚动(battle-buff-list:黄铜细 thumb,见 styles/features/battle.css)。
            // 336 = 原 168 的两倍:CRPG 里 buff 动辄四五条,168 两条半就滚,可读性差(试玩反馈)。
            <div className="battle-buff-list flex flex-col gap-1 pr-1" style={{ maxHeight: 336 }}>
              {buffEntries.map((e) => {
                const styles = TONE_CHIP_STYLES[e.tone];
                const remaining = formatRemaining(e);
                const isSpecial = e.tone === 'special';
                return (
                  <motion.div
                    key={e.id}
                    // 新 buff 出现时浮现(展开条目用 fade-rise,比 chip 的 spring-pop 安静);
                    // 隐藏 buff(仅开发期强显)整条压暗一档,跟正常 buff 区分。
                    initial={{ opacity: 0, y: 4 }}
                    animate={{ opacity: e.hidden ? 0.82 : 1, y: 0 }}
                    transition={{ duration: 0.18, ease: 'easeOut' }}
                    className="relative overflow-hidden px-2 py-1.5 rounded-[3px] shrink-0"
                    // special 的 boxShadow + SVG = 原色低浮雕;其余 tone 仍是平面色块。
                    style={{ border: styles.border, background: styles.background, boxShadow: styles.boxShadow }}
                  >
                    {isSpecial && <SpecialBuffSmoke rarity={e.rarity} />}

                    {/* 标题行:tone dot + 名称 + 剩余回合(右对齐) */}
                    <div className="relative z-10 flex items-center gap-1.5">
                      <span
                        className="size-1.5 rounded-full shrink-0"
                        style={{ background: styles.dot, boxShadow: styles.dotShadow }}
                        aria-hidden
                      />
                      <span
                        className="text-[11px] font-semibold text-parchment truncate font-[family-name:var(--font-display)]"
                        style={isSpecial ? {
                          color: '#ddcda4',
                          textShadow: '-0.6px -0.6px 0 rgba(246, 229, 190, 0.1), 0.8px 0.8px 0 rgba(0, 0, 0, 0.8)',
                          letterSpacing: '0.025em',
                        } : undefined}
                      >
                        {e.name}
                      </span>
                      {e.hidden && (
                        <EyeOff
                          className="size-3 shrink-0 text-parchment-dark/60"
                          strokeWidth={1.8}
                          aria-label="开发期强制显示的隐藏 buff（Meta.Hidden）"
                        />
                      )}
                      {remaining && (
                        <span className="ml-auto shrink-0 text-[9px] tabular-nums text-parchment-dark/70 font-[family-name:var(--font-subtitle)]">
                          {remaining}
                        </span>
                      )}
                    </div>
                    {/* 描述 —— 直接展开,走 RichText 解析 `{keyword.id}` 占位符出 nested
                        tooltip(同技能卡描述),古书条目质感走 subtitle 混合栈。buff 文案里的
                        {'{Abnormal.Reveal}'} 等术语会渲染成可 hover 的关键词。 */}
                    {e.description && (
                      <div className="relative z-10 text-[10px] leading-snug text-parchment-dark/85 mt-0.5 pl-3 whitespace-pre-line break-words font-[family-name:var(--font-subtitle)]">
                        <RichText text={e.description} keywords={BATTLE_KEYWORDS} styles={BATTLE_RICHTEXT_STYLES} />
                      </div>
                    )}
                    {/* 属性修改 —— 逐条 "属性名 +N",数值按 tone ink 染色(battle-palette) */}
                    {e.modifiers.length > 0 && (
                      <div className="relative z-10 flex flex-wrap gap-x-2 gap-y-0.5 mt-0.5 pl-3">
                        {e.modifiers.map((m, i) => (
                          <span key={i} className="text-[9.5px] tabular-nums whitespace-nowrap font-[family-name:var(--font-subtitle)]">
                            <span className="text-parchment-dark/70">{m.attributeLabel} </span>
                            <span className="font-semibold" style={{ color: TONE_INK[m.tone] }}>
                              {m.displayValue}
                            </span>
                          </span>
                        ))}
                      </div>
                    )}
                  </motion.div>
                );
              })}
            </div>
          )}
        </motion.div>
      )}
    </AnimatePresence>
  );
}