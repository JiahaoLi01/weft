import { useEffect, useMemo, useRef, useState } from 'react';
import { usePick17Store, type Pick17Character, type Pick17Option } from '@/stores/pick17-store';
import { useUIStore } from '@/stores/ui-store';
import { sendToUE } from '@/lib/ue-bridge';
import { AbilityIcon } from '@/components/battle/ability-icons';
import { RichText } from '@/components/NestedTooltip';
import { SmokeBg } from '@/components/SmokeBg';
import { SPECIAL_SMOKE_PARAMS } from '@/components/BattleUnitStatusPanel';
import { BATTLE_KEYWORDS } from '@/config/battle-keywords';
import { BATTLE_RICHTEXT_STYLES } from '@/config/battle-richtext-styles';
import { ANTIQUE_GOLD, BAR_GROUND, NAME_INK, brass, garnet } from '@/config/battle-palette';

/**
 * Pick 17 演示模式 —— 开局技能构筑选择页。
 *
 * C++ `UPick17BattleSchema` 在战斗开局 PushUI('pick17') 后推 `setPick17Draft`(store)。
 * 玩家用**全队共享**的 totalPoints(典型 17)在所有角色的候选池间自由分配;确认 →
 * `battleUI.pick17Confirm` 回传 C++ 重初始化角色,页面自行 pop。
 *
 * 卡片**复用战斗技能面板的语言**:标题/描述走 RichText(`{keyword}` 嵌套 tooltip +
 * `{style.*}` 混排字体);稀有度**复用 buff 面板的烟雾特效**(SpecialBuffSmoke 同款:
 * SmokeBg + SmokeBuffLab 调定参数,烟色按稀有度 RARITY_RGB 灰/蓝/紫/琥珀)——未配稀有度
 * 即普通显示(无烟);目录分组读技能本体的 CategoryTag(法术/技巧/装备/物品四根,层级
 * 细分如「法术 · Evocation」)。池子只配父技能 —— 父技能卡列出全部变体(买父带全家)。
 * Ultralight 约束:hover 全走 JS state;烟雾 canvas 分辨率压低(SmokeBg 自带离屏停算)。
 */

/** 目录根(Ability.Category.<Root>)→ 中文组名。未映射/缺省 → 其他。 */
const CATEGORY_ROOT_LABEL: Record<string, string> = {
  Spell: '法术',
  Technique: '技巧',
  Equipment: '装备',
  Item: '物品',
  Talent: '天赋',
};

const FALLBACK_CATEGORY = '其他';
const CATEGORY_PREFIX = 'Ability.Category.';

/**
 * 视野机制档位 —— 与 C++ EPick17VisionMode / FPick17ConfirmedPayload.VisionMode 对齐,
 * 随确认载荷回传(`visionMode` 字段)。
 */
const VISION_MODES = [
  { id: 'default', name: '默认', desc: '视野规则保持原样' },
  { id: 'classic', name: '经典', desc: '出现在敌对单位视野中的角色获得 1 回合揭示' },
  { id: 'action', name: '动作', desc: '任何角色使用动作后暴露自身,持续到回合结束' },
  { id: 'open', name: '明牌', desc: '视线无视墙体遮挡;烟雾等条件遮蔽依然有效' },
] as const;

type VisionModeId = (typeof VISION_MODES)[number]['id'];

/** 单个视野档位卡(radio 语义)。JS hover(Ultralight 约束)。 */
function VisionModeCard({
  mode,
  selected,
  onSelect,
}: {
  mode: (typeof VISION_MODES)[number];
  selected: boolean;
  onSelect: () => void;
}) {
  const [hovered, setHovered] = useState(false);
  return (
    <div
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
      onClick={onSelect}
      className="flex-1 min-w-0 px-2.5 py-2 select-none cursor-pointer transition-colors duration-150 border"
      style={{
        background: selected ? brass(0.16) : hovered ? 'rgba(233, 221, 193, 0.06)' : 'rgba(233, 221, 193, 0.025)',
        borderColor: brass(selected ? 0.8 : hovered ? 0.45 : 0.22),
      }}
    >
      <div className="flex items-center gap-2">
        {/* radio 圆点:选中实心黄铜,未选空环 */}
        <span
          className="shrink-0 rounded-full"
          style={{
            width: 8,
            height: 8,
            background: selected ? brass(0.9) : 'transparent',
            border: `1px solid ${brass(selected ? 0.9 : 0.45)}`,
          }}
        />
        <span
          className={[
            'text-[13px] tracking-[0.1em] font-[family-name:var(--font-display)]',
            selected ? 'text-gold-light' : 'text-parchment',
          ].join(' ')}
        >
          {mode.name}
        </span>
      </div>
      <div className="text-[10px] leading-snug mt-1 text-parchment-dark/80 font-[family-name:var(--font-subtitle)]">
        {mode.desc}
      </div>
    </div>
  );
}

/** 完整目录 tag → 组标签:根译中文,更深层级原样拼接(「法术 · Evocation」)。 */
function categoryLabel(tag?: string): string {
  if (!tag) return FALLBACK_CATEGORY;
  const rest = tag.startsWith(CATEGORY_PREFIX) ? tag.slice(CATEGORY_PREFIX.length) : tag;
  const segments = rest.split('.').filter(Boolean);
  if (segments.length === 0) return FALLBACK_CATEGORY;
  const root = CATEGORY_ROOT_LABEL[segments[0]] ?? segments[0];
  return segments.length > 1 ? `${root} · ${segments.slice(1).join(' · ')}` : root;
}

type Picked = Record<string, ReadonlySet<string>>;

function OptionCard({
  option,
  selected,
  affordable,
  onToggle,
}: {
  option: Pick17Option;
  selected: boolean;
  /** 未选中且勾上会超支时 false —— 卡片压暗,点击无效。 */
  affordable: boolean;
  onToggle: () => void;
}) {
  const [hovered, setHovered] = useState(false);
  const clickable = selected || affordable;

  return (
    <div
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
      onClick={() => {
        if (clickable) onToggle();
      }}
      className={[
        'relative overflow-hidden flex gap-2 px-2.5 py-2 select-none transition-colors duration-150 border',
        clickable ? 'cursor-pointer' : 'cursor-not-allowed opacity-50',
      ].join(' ')}
      style={{
        background: selected
          ? brass(0.16)
          : hovered && clickable
            ? 'rgba(233, 221, 193, 0.06)'
            : 'rgba(233, 221, 193, 0.025)',
        borderColor: brass(selected ? 0.8 : hovered && clickable ? 0.45 : 0.22),
      }}
    >
      {/* 稀有度 = buff 面板同款烟雾(从右向左,SmokeBuffLab 调定参数,烟色按稀有度)。
          未配稀有度不渲染 —— 普通显示。卡片比 buff chip 宽高得多:铺满会糊成一面色墙,
          约束到右段(chip 同比例)+ 降一档不透明度,还原「右缘一缕淡烟」的原始观感。 */}
      {option.rarity && (
        <div className="absolute inset-y-0 right-0 pointer-events-none" style={{ width: '58%', opacity: 0.55 }}>
          <SmokeBg
            rarity={option.rarity}
            params={SPECIAL_SMOKE_PARAMS}
            fade="right"
            rotate90
            resolution={{ w: 128, h: 32 }}
            className="absolute inset-0 w-full h-full"
          />
        </div>
      )}

      <div
        className="relative shrink-0 mt-0.5"
        style={{
          width: 34,
          height: 34,
          color: selected ? '#e8d9b0' : ANTIQUE_GOLD,
          border: `1px solid ${brass(selected ? 0.6 : 0.3)}`,
          background: BAR_GROUND,
          padding: 4,
        }}
      >
        <AbilityIcon name={option.icon} className="w-full h-full" />
      </div>

      <div className="relative flex-1 min-w-0">
        <div className="flex items-start justify-between gap-2">
          {/* 标题走 RichText(混排字体 token / 关键词)—— 稀有度信号由烟雾承载,标题不染色 */}
          <span
            className={[
              'text-sm font-[family-name:var(--font-display)] transition-colors',
              selected ? 'text-gold-light' : 'text-parchment',
            ].join(' ')}
          >
            <RichText text={option.name} keywords={BATTLE_KEYWORDS} styles={BATTLE_RICHTEXT_STYLES} />
          </span>

          {/* 点数章 —— 选中态反白;不可负担态染石榴红(「买不起」信号,与 cost chip 语义一致) */}
          <span
            className="shrink-0 text-[11px] tracking-[0.06em] px-1.5 py-0.5 whitespace-nowrap"
            style={{
              color: selected ? '#171208' : !affordable && !selected ? '#cf8e86' : ANTIQUE_GOLD,
              background: selected ? brass(0.85) : 'transparent',
              border: `1px solid ${selected ? brass(0.9) : !affordable && !selected ? garnet(0.6) : brass(0.4)}`,
            }}
          >
            {option.points} 点
          </span>
        </div>

        {/* 描述 —— 战斗面板同款 RichText(`{keyword}` 悬停出嵌套 tooltip);保留设计师换行 */}
        {option.description && (
          <div
            className={[
              'text-[10.5px] whitespace-pre-line break-words leading-snug mt-0.5',
              'font-[family-name:var(--font-subtitle)]',
              selected ? 'text-parchment/95' : 'text-parchment-dark/85',
            ].join(' ')}
          >
            <RichText text={option.description} keywords={BATTLE_KEYWORDS} styles={BATTLE_RICHTEXT_STYLES} />
          </div>
        )}

        {/* 变体展开区 —— 池子只配父技能,这里**完整展开**「买它一并获得」的全部变体
            (图标 + 名字 + 描述,玩家选购前就能看清每个变体做什么;图标空则沿用父纹章)。
            左侧黄铜细线做引文式缩进,与父技能描述在视觉上分层。 */}
        {option.variants && option.variants.length > 0 && (
          <div
            className="mt-1.5 pl-2 flex flex-col gap-1.5"
            style={{ borderLeft: `1px solid ${brass(0.28)}` }}
          >
            <span className="text-[9.5px] tracking-[0.2em]" style={{ color: brass(0.55) }}>
              变体
            </span>
            {option.variants.map((variant, index) => (
              <div key={index} className="flex gap-1.5 min-w-0">
                <AbilityIcon
                  name={variant.icon || undefined}
                  fallbackName={option.icon}
                  className="size-4 shrink-0 mt-[1px] text-gold/85"
                />
                <div className="flex-1 min-w-0">
                  <div className="text-[11px] leading-tight text-parchment/95 font-[family-name:var(--font-display)]">
                    <RichText text={variant.name} keywords={BATTLE_KEYWORDS} styles={BATTLE_RICHTEXT_STYLES} />
                  </div>
                  {variant.description && (
                    <div className="text-[10px] whitespace-pre-line break-words leading-snug mt-0.5 text-parchment-dark/80 font-[family-name:var(--font-subtitle)]">
                      <RichText text={variant.description} keywords={BATTLE_KEYWORDS} styles={BATTLE_RICHTEXT_STYLES} />
                    </div>
                  )}
                </div>
              </div>
            ))}
          </div>
        )}
      </div>
    </div>
  );
}

function CharacterSection({
  character,
  picked,
  remaining,
  onToggle,
}: {
  character: Pick17Character;
  picked: ReadonlySet<string>;
  remaining: number;
  onToggle: (abilityId: string, points: number) => void;
}) {
  // 按目录标签分组成段;组内保持 C++ 下发顺序(即资产配置顺序),「其他」殿后。
  const groups = useMemo(() => {
    const map = new Map<string, Pick17Option[]>();
    for (const option of character.options) {
      const key = categoryLabel(option.category);
      const arr = map.get(key) ?? [];
      arr.push(option);
      map.set(key, arr);
    }
    const entries = [...map.entries()];
    entries.sort((a, b) => (a[0] === FALLBACK_CATEGORY ? 1 : b[0] === FALLBACK_CATEGORY ? -1 : 0));
    return entries;
  }, [character.options]);

  const spentHere = character.options.reduce(
    (sum, o) => sum + (picked.has(o.id) ? o.points : 0), 0);

  return (
    <section style={{ marginBottom: 26 }}>
      <div style={{ display: 'flex', alignItems: 'baseline', gap: 12, marginBottom: 10 }}>
        <h2
          style={{
            color: NAME_INK,
            fontSize: 17,
            letterSpacing: '0.1em',
            fontFamily: 'var(--font-subtitle, serif)',
          }}
        >
          {character.name}
        </h2>
        <span style={{ color: brass(0.65), fontSize: 11, letterSpacing: '0.08em' }}>
          已选 {picked.size} 项 · {spentHere} 点
        </span>
        <div style={{ flex: 1, height: 1, background: brass(0.25) }} />
      </div>

      {groups.map(([category, options]) => (
        <div key={category} style={{ marginBottom: 12 }}>
          <div
            style={{
              color: brass(0.7),
              fontSize: 10.5,
              letterSpacing: '0.22em',
              marginBottom: 6,
            }}
          >
            {category}
          </div>
          <div
            style={{
              display: 'grid',
              gridTemplateColumns: 'repeat(auto-fill, minmax(320px, 1fr))',
              gap: 8,
            }}
          >
            {options.map((option) => {
              const selected = picked.has(option.id);
              return (
                <OptionCard
                  key={option.id}
                  option={option}
                  selected={selected}
                  affordable={option.points <= remaining}
                  onToggle={() => onToggle(option.id, option.points)}
                />
              );
            })}
          </div>
        </div>
      ))}
    </section>
  );
}

export function Pick17Draft() {
  const draft = usePick17Store((s) => s.draft);
  const [picked, setPicked] = useState<Picked>({});
  const [visionMode, setVisionMode] = useState<VisionModeId>('default');
  const [confirmHover, setConfirmHover] = useState(false);
  const [isVisible, setIsVisible] = useState(false);
  const [isClosing, setIsClosing] = useState(false);
  const closeTimer = useRef<ReturnType<typeof setTimeout> | null>(null);

  console.log('[Pick17] Pick17Draft render attempt', {
    hasDraft: !!draft,
    characterCount: draft?.characters?.length ?? 0,
  });

  // Page starts transparent, then fades in on the next rendered frame. This
  // also makes a missing/covered page evident in the forwarded JS console log.
  useEffect(() => {
    const frame = requestAnimationFrame(() => {
      console.log('[Pick17] Page fade-in started');
      setIsVisible(true);
    });
    return () => cancelAnimationFrame(frame);
  }, []);

  useEffect(() => {
    if (!draft) {
      console.warn('[Pick17] Pick17Draft mounted without a draft; page renders null');
      return;
    }

    console.log('[Pick17] Pick17Draft mounted with draft', {
      totalPoints: draft.totalPoints,
      characterCount: draft.characters.length,
      optionCount: draft.characters.reduce((count, character) => count + character.options.length, 0),
    });
  }, [draft]);

  useEffect(() => () => {
    if (closeTimer.current) {
      clearTimeout(closeTimer.current);
    }
  }, []);

  // Draft 被整体替换(新一场战斗重推 / dev HMR 重灌 mock)而组件未卸载时,旧勾选对新池子
  // 是脏数据(id 可能已不存在或点数变了)—— 换 draft 即清空重选(视野档一并回默认)。
  // 正常游戏流(确认→pop→下场战斗再 push)靠卸载天然干净,这里是防御。
  useEffect(() => {
    setPicked({});
    setVisionMode('default');
  }, [draft]);

  const spent = useMemo(() => {
    if (!draft) return 0;
    let total = 0;
    for (const character of draft.characters) {
      const set = picked[character.personaId];
      if (!set) continue;
      for (const option of character.options) {
        if (set.has(option.id)) total += option.points;
      }
    }
    return total;
  }, [draft, picked]);

  if (!draft) {
    // C++ 还没推 payload(或 dev 直接 pushUI 进来)—— 渲染空壳,别崩。
    return null;
  }

  const remaining = draft.totalPoints - spent;
  const overBudget = remaining < 0;
  const pickedCount = Object.values(picked).reduce((n, s) => n + s.size, 0);

  const toggle = (personaId: string, abilityId: string) => {
    setPicked((prev) => {
      const current = new Set(prev[personaId] ?? []);
      if (current.has(abilityId)) {
        current.delete(abilityId);
      } else {
        current.add(abilityId);
      }
      return { ...prev, [personaId]: current };
    });
  };

  const confirm = () => {
    if (overBudget || isClosing) return;
    const selections = draft.characters
      .map((c) => ({ personaId: c.personaId, abilityIds: [...(picked[c.personaId] ?? [])] }))
      .filter((s) => s.abilityIds.length > 0);
    console.log('[Pick17] Confirmed; page fade-out started', { selectionCount: selections.length, visionMode });
    setIsClosing(true);

    // Keep the page mounted until its 0.5 s exit transition finishes.
    // This avoids the battle HUD replacing it in the same frame as confirmation.
    closeTimer.current = setTimeout(() => {
      console.log('[Pick17] Fade-out completed; closing page');
      useUIStore.getState().pop();
      sendToUE('battleUI.pick17Confirm', { selections, visionMode });
    }, 500);
  };

  return (
    <div
      style={{
        position: 'absolute',
        inset: 0,
        background: 'rgba(13, 10, 7, 0.93)',
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'center',
        opacity: isVisible && !isClosing ? 1 : 0,
        pointerEvents: isClosing ? 'none' : 'auto',
        transition: 'opacity 500ms ease-in-out',
      }}
    >
      <div
        style={{
          width: 'min(1080px, 92vw)',
          display: 'flex',
          flexDirection: 'column',
          height: '100%',
          padding: '28px 0 20px',
        }}
      >
        {/* ── 页眉:标题 + 共享预算表 ─────────────────────────────── */}
        <header style={{ display: 'flex', alignItems: 'flex-end', gap: 18, marginBottom: 6 }}>
          <div>
            <h1
              style={{
                color: NAME_INK,
                fontSize: 24,
                letterSpacing: '0.18em',
                fontFamily: 'var(--font-subtitle, serif)',
              }}
            >
              开战准备
            </h1>
            <div style={{ color: brass(0.6), fontSize: 11, letterSpacing: '0.14em', marginTop: 4 }}>
              以 {draft.totalPoints} 点为限,为全队自由拣选开战技能 —— 点数全队共享
            </div>
          </div>
          <div style={{ flex: 1 }} />
          <div style={{ textAlign: 'right' }}>
            <div
              style={{
                fontSize: 30,
                fontFamily: 'var(--font-subtitle, serif)',
                letterSpacing: '0.06em',
                color: overBudget ? '#cf8e86' : ANTIQUE_GOLD,
              }}
            >
              {spent}
              <span style={{ fontSize: 15, color: brass(0.55) }}> / {draft.totalPoints} 点</span>
            </div>
            {/* 预算条:花费占比;超支整条转石榴红 */}
            <div style={{ width: 220, height: 4, background: brass(0.18), marginTop: 6 }}>
              <div
                style={{
                  width: `${Math.min(100, (spent / draft.totalPoints) * 100)}%`,
                  height: '100%',
                  background: overBudget ? garnet(0.85) : brass(0.8),
                  transition: 'width 140ms',
                }}
              />
            </div>
          </div>
        </header>

        <div style={{ height: 1, background: brass(0.3), marginBottom: 12 }} />

        {/* ── 视野机制(radio 档位,随确认载荷回传 visionMode) ────────── */}
        <div style={{ marginBottom: 14 }}>
          <div
            style={{
              color: brass(0.7),
              fontSize: 10.5,
              letterSpacing: '0.22em',
              marginBottom: 6,
            }}
          >
            视野机制
          </div>
          <div style={{ display: 'flex', gap: 8 }}>
            {VISION_MODES.map((mode) => (
              <VisionModeCard
                key={mode.id}
                mode={mode}
                selected={visionMode === mode.id}
                onSelect={() => setVisionMode(mode.id)}
              />
            ))}
          </div>
        </div>

        {/* ── 角色分节(滚动区) ───────────────────────────────────── */}
        <div className="pick17-scroll-area" style={{ flex: 1, overflowY: 'auto', paddingRight: 6 }}>
          {draft.characters.map((character) => (
            <CharacterSection
              key={character.personaId}
              character={character}
              picked={picked[character.personaId] ?? new Set()}
              remaining={remaining}
              onToggle={(abilityId) => toggle(character.personaId, abilityId)}
            />
          ))}
        </div>

        {/* ── 页脚:剩余点数 + 确认 ───────────────────────────────── */}
        <footer
          style={{
            display: 'flex',
            alignItems: 'center',
            gap: 16,
            paddingTop: 14,
            borderTop: `1px solid ${brass(0.3)}`,
          }}
        >
          <span style={{ color: overBudget ? '#cf8e86' : brass(0.7), fontSize: 12, letterSpacing: '0.1em' }}>
            {overBudget ? `超支 ${-remaining} 点 —— 移除部分选择` : `剩余 ${remaining} 点 · 共 ${pickedCount} 项`}
          </span>
          <div style={{ flex: 1 }} />
          <button
            onMouseEnter={() => setConfirmHover(true)}
            onMouseLeave={() => setConfirmHover(false)}
            onClick={confirm}
            disabled={overBudget || isClosing}
            style={{
              padding: '10px 34px',
              fontSize: 14,
              letterSpacing: '0.24em',
              fontFamily: 'var(--font-subtitle, serif)',
              color: overBudget ? 'rgba(233, 221, 193, 0.3)' : confirmHover ? '#171208' : ANTIQUE_GOLD,
              background: overBudget ? 'transparent' : confirmHover ? brass(0.85) : brass(0.1),
              border: `1px solid ${overBudget ? brass(0.25) : brass(0.75)}`,
              cursor: overBudget ? 'default' : 'pointer',
              transition: 'background 120ms, color 120ms',
            }}
          >
            确认出战
          </button>
        </footer>
      </div>
    </div>
  );
}
