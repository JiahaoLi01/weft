import {
  useState, useRef, useCallback, useEffect,
  createContext, useContext,
} from 'react';
import { createPortal } from 'react-dom';
import { SmokeBg, type SmokeParams } from './SmokeBg';

// ===== Types =====

export type TooltipCategory = 'term' | 'equipment' | 'spell' | 'status' | 'buff';
export type Rarity = 'common' | 'rare' | 'epic' | 'legendary';

/**
 * Buff 类 tooltip 的运行时数据 —— BattleUnitStatusPanel 把每条 buff 翻译成 KeywordDef
 * (category='buff')时通过 `buffMeta` 把这些字段一起塞进来。`stats` 仍然用来展示
 * "属性 → 修改值" 的列表;`buffMeta` 是除属性表外的额外 buff 元数据(剩余时间 / 层数 / 类型)。
 *
 * 之所以不复用 `stats` 装"剩余时间"等元数据:它们语义不同 —— stats 是"buff 对角色属性
 * 的具体影响",元数据是"buff 自身的生命周期信息"。视觉上分两块更清楚。
 */
/**
 * 单条属性修改器在 tooltip 里的呈现。`displayValue` 由调用方预先格式化好(已经带 +/−/×/=
 * 符号、% 单位等),tooltip 只负责按 tone 着色。
 *
 * `tone`:
 *   - 'positive' → 绿(数值增益)
 *   - 'negative' → 红(数值削减)
 *   - 'neutral'  → 默认色(ForceSet 这种"既不增也不减"的场景)
 *
 * 跟 KeywordDef.stats(equipment tooltip 用的扁平 Record)不复用,因为 stats 没有 tone 这层
 * 语义 —— equipment 的 "+15%" 跟 "+5" 都是中性数据展示。buff 这边一目了然的红绿区分是
 * 战斗体验的硬需求,值得专门的数据通道。
 */
export interface BuffTooltipModifier {
  attributeLabel: string;
  displayValue: string;
  tone: 'positive' | 'negative' | 'neutral';
}

export interface BuffTooltipMeta {
  /** Stack 数;1 时不显示 */
  stackCount?: number;
  /**
   * 剩余回合数(单位契约:回合,原值直推,见 battle-store BattleBuff)。
   * Limited 类才有意义;< 0 表示 Infinity / Immediately,UI 显示"永久"或干脆不显示。
   */
  remainingRounds?: number;
  lifetimeType?: 'Limited' | 'Infinity' | 'Immediately';
  /** 属性修改器列表。BuffTooltip 渲染器据此画表格。 */
  modifiers?: BuffTooltipModifier[];
}

export interface KeywordDef {
  id: string;
  name: string;
  description: string; // may contain {keyword_id} markers
  color: string;       // tailwind text color class, e.g. "text-red-400"

  /** Determines which tooltip renderer is used. Defaults to 'term'. */
  category?: TooltipCategory;

  // Equipment / Spell fields (used when category matches)
  icon?: string;
  rarity?: Rarity;
  stats?: Record<string, string>;

  // Buff fields (used when category === 'buff')
  buffMeta?: BuffTooltipMeta;
}

export interface ItemDef {
  id: string;
  name: string;
  description: string;
  rarity: Rarity;
  icon: string;
  stats?: Record<string, string>;
}

interface PinnedEntry {
  uid: number;
  keyword: KeywordDef;
  x: number;
  y: number;
}

const MAX_DEPTH = 3;
const PIN_DELAY_MS = 3000;
const TOOLTIP_W = 256;
const TOOLTIP_H_EST = 120; // estimated tooltip height for bounds check
const PINNED_W = 288;      // w-72
const PINNED_H_EST = 140;
const MARGIN = 12;

/** Clamp a tooltip position to stay within viewport (zoom-aware) */
function clampToViewport(
  x: number, y: number, w: number, h: number,
  anchorRect?: DOMRect,
): { x: number; y: number } {
  // Under CSS zoom, innerWidth/Height are physical pixels but
  // fixed positioning uses logical coords. Divide by zoom to get logical viewport.
  const zoom = getZoom();
  const vw = window.innerWidth / zoom;
  const vh = window.innerHeight / zoom;

  // Horizontal: prefer right of anchor, flip left if overflow
  if (x + w > vw - MARGIN) {
    x = anchorRect ? anchorRect.left - w - 8 : vw - w - MARGIN;
  }
  if (x < MARGIN) {
    x = MARGIN;
  }

  // Vertical: clamp to viewport
  if (y + h > vh - MARGIN) {
    y = vh - h - MARGIN;
  }
  if (y < MARGIN) {
    y = MARGIN;
  }

  return { x, y };
}

const RARITY_STYLES: Record<string, string> = {
  common:    'border-zinc-500 text-zinc-300',
  rare:      'border-blue-400 text-blue-400',
  epic:      'border-purple-400 text-purple-400',
  legendary: 'border-amber-400 text-amber-400',
};

// ===== Pinned Tooltip Context =====

interface PinnedCtx {
  pinned: PinnedEntry[];
  keywords: Map<string, KeywordDef>;
  pin: (keyword: KeywordDef, x: number, y: number) => void;
  unpin: (uid: number) => void;
  unpinAll: () => void;
}

const PinnedContext = createContext<PinnedCtx>({
  pinned: [],
  keywords: new Map(),
  pin: () => {},
  unpin: () => {},
  unpinAll: () => {},
});

let nextPinUid = 1;
let nextZIndex = 100;

function getZoom(): number {
  const z = parseFloat(document.documentElement.style.zoom || '1');
  return z > 0 ? z : 1;
}

/**
 * 暴露 PinnedTooltip 的命令式 API 给 Provider 外的消费者。
 *
 * 典型用例:某个 panel(如 BattleUnitPanel)在它自己退场时,主动 unpinAll 收掉它产生的
 * pinned tooltip。理由:WebUI 这套环境里"点空白 dismiss"基于 `window mousedown`,但事件
 * 只在点击点 cursor 非默认时才会送到 JS —— panel 退场后屏幕上没别的非默认 cursor 元素,
 * 点击全部漏到游戏世界,outside-click dismiss 实际不工作。各 panel 自己兜底退场清理是最
 * 简单的修法。
 *
 * 这是有意保留"跨页 pin"的语义:玩家从 BattleUnitPanel 钉了一条术语,切到 Settings 还能
 * 看见;但 selectedUnit 变 null(战斗结束 / 切了别的单位)就清。要更彻底的清理可以监听
 * 路由变化或加 Esc 键 binding。
 *
 * 注:`react-refresh/only-export-components` 规则在这个文件里对 hook 报警 —— hook 跟一组
 * components(NestedTooltipLayer / KeywordSpan / RichText / ItemCard / PinnedTooltipProvider)
 * 共处一文件,违反"only export components"原则。要彻底满足规则得把 hook 拆到独立文件,
 * 改动面比 disable 一行大很多。下面 inline disable 是局部解。
 */
// eslint-disable-next-line react-refresh/only-export-components
export function usePinnedTooltip() {
  return useContext(PinnedContext);
}

export function PinnedTooltipProvider({
  keywords,
  children,
}: {
  keywords: Map<string, KeywordDef>;
  children: React.ReactNode;
}) {
  const [pinned, setPinned] = useState<PinnedEntry[]>([]);

  const pin = useCallback((keyword: KeywordDef, x: number, y: number) => {
    setPinned(prev => [...prev, { uid: nextPinUid++, keyword, x, y }]);
  }, []);

  const unpin = useCallback((uid: number) => {
    setPinned(prev => prev.filter(p => p.uid !== uid));
  }, []);

  const unpinAll = useCallback(() => {
    setPinned([]);
  }, []);

  return (
    <PinnedContext.Provider value={{ pinned, keywords, pin, unpin, unpinAll }}>
      {children}
    </PinnedContext.Provider>
  );
}

// ===== Parser =====

/**
 * 标记段类型:
 *   - `text`:普通文本
 *   - `keyword`:`{keyword.id}` —— 出 hover tooltip 的术语链接
 *   - `style`:`{style.name:inner text}` —— 内联样式片段。`name` 由调用方提供的 styles map
 *     映射到 CSS class(典型用例:维多利亚商品名混合字体,Cinzel 大写 brand + Cinzel 数字
 *     口径 + EB Garamond 斜体 flourish)
 *
 * **不支持嵌套**:`{style.x:含 {keyword.y} 链接}` 会被解析器在内层 `}` 截断 —— 设计师
 * 想要"有样式又是链接"目前要么挑一种,要么写两个并列 token(`{keyword.y}` 旁边再 inline
 * 一个 `{style.x:...}`)。要支持嵌套就得换成递归 parser,当前 demo 阶段单层够用。
 */
type Segment =
  | { type: 'text'; value: string }
  | { type: 'keyword'; id: string }
  | { type: 'style'; name: string; inner: string };

function parseDescription(text: string): Segment[] {
  const segments: Segment[] = [];
  // 双形态正则:
  //   `{id}`          → keyword segment(id 是 `Status.Stunned` / `Resource.BasicResource.MainAction` 等)
  //   `{id:inner}`    → style segment(inner 是要被样式包裹的可见文本)
  // 第 2 个捕获组带 `?` —— 没冒号就是 undefined,有就是冒号后的文本(到 `}` 为止,不含 `}`)。
  //
  // id 字符集 `[\w.-]+` 允许字母 / 数字 / 下划线 / 点 / 短横,匹配项目 keyword
  // (`Status.Stunned`)+ style 名(`style.victorian-display`)两种命名风格。
  const regex = /\{([\w.-]+)(?::([^}]*))?\}/g;
  let lastIndex = 0;
  let match: RegExpExecArray | null;

  while ((match = regex.exec(text)) !== null) {
    if (match.index > lastIndex) {
      segments.push({ type: 'text', value: text.slice(lastIndex, match.index) });
    }
    const id = match[1];
    const inner = match[2];
    if (inner !== undefined) {
      // `{id:inner}` 形式。仅 `style.` 命名空间识别为样式段;其它带冒号但不是 `style.` 前缀
      // 的当 plain text 回退(让 designer 看到"啊我写错了"而不是默默吞掉)。
      if (id.startsWith('style.')) {
        segments.push({ type: 'style', name: id.slice('style.'.length), inner });
      } else {
        segments.push({ type: 'text', value: match[0] });
      }
    } else {
      // `{id}` 形式 —— keyword 链接(老语义,完全兼容)
      segments.push({ type: 'keyword', id });
    }
    lastIndex = regex.lastIndex;
  }

  if (lastIndex < text.length) {
    segments.push({ type: 'text', value: text.slice(lastIndex) });
  }

  return segments;
}

// ===== RichText =====

/**
 * 富文本渲染。语法:
 *   - `{keyword.id}`          → 术语链接,hover 出 tooltip(走 keywords prop 查表)
 *   - `{style.name:text}`     → 内联样式片段,CSS class 来自 styles prop 查表(可选)
 *   - 其它纯文本 → 原样渲染
 *
 * `styles` 可空。空时 `{style.*:...}` 会回退到 plain text(inner 内容)+ console.warn,
 * 让 designer 看到"配错"而不是默默吞掉。同样 keyword 没在 keywords map 里也走灰色 fallback。
 */
export function RichText({
  text,
  keywords,
  styles,
  depth = 0,
}: {
  text: string;
  keywords: Map<string, KeywordDef>;
  /** style name → CSS class string。可选;不传则 `{style.*:*}` token 走 plain-text 回退。 */
  styles?: Record<string, string>;
  depth?: number;
}) {
  const segments = parseDescription(text);

  return (
    <>
      {segments.map((seg, i) => {
        if (seg.type === 'text') {
          return <span key={i}>{seg.value}</span>;
        }
        if (seg.type === 'style') {
          const cls = styles?.[seg.name];
          if (!cls) {
            // 未配置的 style 名 —— 灰色显示 raw token 让 designer 一眼看出哪里错了。
            // 跟 keyword 缺失走的同款 visual cue。
            return <span key={i} className="text-zinc-500">[style.{seg.name}:{seg.inner}]</span>;
          }
          return <span key={i} className={cls}>{seg.inner}</span>;
        }
        // keyword
        const kw = keywords.get(seg.id);
        if (!kw) {
          return <span key={i} className="text-zinc-500">[{seg.id}]</span>;
        }
        return (
          <KeywordSpan key={i} keyword={kw} keywords={keywords} depth={depth} />
        );
      })}
    </>
  );
}

// ===== KeywordSpan =====

/**
 * KeywordSpan —— hover-to-tooltip 的最小 building block。
 *
 * 之前只在 RichText 内部用(从字符串里解析占位符出来),现在 BattleUnitStatusPanel 等
 * 直接拼装 KeywordDef 的场景也直接用 —— 不走 string parse,直接拿 def 渲染。
 */
export function KeywordSpan({
  keyword,
  keywords,
  depth,
}: {
  keyword: KeywordDef;
  keywords: Map<string, KeywordDef>;
  depth: number;
}) {
  const [open, setOpen] = useState(false);
  const [pinning, setPinning] = useState(false);
  const hideTimer = useRef<ReturnType<typeof setTimeout>>(null);
  const pinTimer = useRef<ReturnType<typeof setTimeout>>(null);
  const spanRef = useRef<HTMLSpanElement>(null);
  const { pin } = useContext(PinnedContext);

  const clearTimers = useCallback(() => {
    if (hideTimer.current) { clearTimeout(hideTimer.current); hideTimer.current = null; }
    if (pinTimer.current) { clearTimeout(pinTimer.current); pinTimer.current = null; }
    setPinning(false);
  }, []);

  const show = useCallback(() => {
    clearTimers();
    setOpen(true);
    setPinning(true);

    pinTimer.current = setTimeout(() => {
      if (spanRef.current) {
        const rect = spanRef.current.getBoundingClientRect();
        const pos = clampToViewport(rect.right + 8, rect.top, PINNED_W, PINNED_H_EST, rect);
        pin(keyword, pos.x, pos.y);
        setOpen(false);
        setPinning(false);
      }
    }, PIN_DELAY_MS);
  }, [clearTimers, keyword, pin]);

  const hide = useCallback(() => {
    if (pinTimer.current) { clearTimeout(pinTimer.current); pinTimer.current = null; }
    setPinning(false);
    hideTimer.current = setTimeout(() => setOpen(false), 150);
  }, []);

  // Click to pin immediately (skip 3s wait)
  const clickToPin = useCallback((e: React.MouseEvent) => {
    e.stopPropagation(); // prevent dismiss-all handler
    e.preventDefault();
    clearTimers();
    if (spanRef.current) {
      const rect = spanRef.current.getBoundingClientRect();
      const pos = clampToViewport(rect.right + 8, rect.top, PINNED_W, PINNED_H_EST, rect);
      pin(keyword, pos.x, pos.y);
      setOpen(false);
    }
  }, [clearTimers, keyword, pin]);

  useEffect(() => clearTimers, [clearTimers]);

  const canNest = depth < MAX_DEPTH;

  return (
    <span
      ref={spanRef}
      data-keyword-span
      className={`relative inline ${keyword.color} font-semibold cursor-help
                  border-b border-dotted border-current`}
      onMouseEnter={canNest ? show : undefined}
      onMouseLeave={canNest ? hide : undefined}
      onClick={canNest ? clickToPin : undefined}
    >
      {keyword.name}
      {open && canNest && (
        <TooltipPopup
          keyword={keyword}
          keywords={keywords}
          depth={depth + 1}
          pinning={pinning}
          anchorRef={spanRef}
          onMouseEnter={show}
          onMouseLeave={hide}
        />
      )}
    </span>
  );
}

// ===== TooltipPopup (portal-based, never clipped) =====

function TooltipPopup({
  keyword,
  keywords,
  depth,
  pinning,
  anchorRef,
  onMouseEnter,
  onMouseLeave,
}: {
  keyword: KeywordDef;
  keywords: Map<string, KeywordDef>;
  depth: number;
  pinning: boolean;
  anchorRef: React.RefObject<HTMLSpanElement | null>;
  onMouseEnter: () => void;
  onMouseLeave: () => void;
}) {
  const [pos, setPos] = useState<{ x: number; y: number } | null>(null);

  useEffect(() => {
    if (!anchorRef.current) return;
    const rect = anchorRef.current.getBoundingClientRect();
    const pos = clampToViewport(rect.right + 8, rect.top - 4, TOOLTIP_W, TOOLTIP_H_EST, rect);
    setPos(pos);
  }, [anchorRef]);

  if (!pos) return null;

  return createPortal(
    <div
      className="w-64 animate-[fadeIn_0.15s_ease-out]"
      style={{ position: 'fixed', left: pos.x, top: pos.y, zIndex: 500 + depth * 10 }}
      onMouseEnter={onMouseEnter}
      onMouseLeave={onMouseLeave}
    >
      <div className="relative">
        <TooltipPanel keyword={keyword} keywords={keywords} depth={depth} />
        {pinning && <PinRing />}
      </div>
    </div>,
    document.body,
  );
}

// ===== Ring progress indicator =====

const RING_SIZE = 20;
const RING_R = 7;
const RING_C = 2 * Math.PI * RING_R;

function PinRing() {
  return (
    <div className="absolute top-1.5 right-1.5" title="Hold to pin">
      <svg width={RING_SIZE} height={RING_SIZE} className="rotate-[-90deg]">
        <circle
          cx={RING_SIZE / 2} cy={RING_SIZE / 2} r={RING_R}
          fill="none" stroke="#3f3f46" strokeWidth={2}
        />
        <circle
          cx={RING_SIZE / 2} cy={RING_SIZE / 2} r={RING_R}
          fill="none" stroke="#a1a1aa" strokeWidth={2}
          strokeDasharray={RING_C}
          strokeDashoffset={RING_C}
          strokeLinecap="round"
          style={{ animation: `pinRingFill ${PIN_DELAY_MS}ms linear forwards` }}
        />
      </svg>
      <div className="absolute inset-0 flex items-center justify-center text-[8px] text-zinc-500">
        &#x1F4CC;
      </div>
    </div>
  );
}

// ===== Tooltip content routing =====

const RARITY_BORDER: Record<string, string> = {
  common:    'border-zinc-500',
  rare:      'border-blue-400',
  epic:      'border-purple-400',
  legendary: 'border-amber-400',
};

function TooltipPanel({
  keyword,
  keywords,
  depth,
}: {
  keyword: KeywordDef;
  keywords: Map<string, KeywordDef>;
  depth: number;
}) {
  const cat = keyword.category ?? 'term';

  if (cat === 'equipment' || cat === 'spell') {
    return (
      <EquipmentTooltip keyword={keyword} keywords={keywords} depth={depth} />
    );
  }

  if (cat === 'status') {
    return (
      <StatusTooltip keyword={keyword} keywords={keywords} depth={depth} />
    );
  }

  if (cat === 'buff') {
    return (
      <BuffTooltip keyword={keyword} keywords={keywords} depth={depth} />
    );
  }

  // Default: term
  return (
    <TermTooltip keyword={keyword} keywords={keywords} depth={depth} />
  );
}

/** Simple glossary/wiki term — current default style */
function TermTooltip({ keyword, keywords, depth }: {
  keyword: KeywordDef; keywords: Map<string, KeywordDef>; depth: number;
}) {
  return (
    <div className="bg-zinc-950 border border-zinc-800 rounded-lg p-3 shadow-lg">
      <div className={`text-sm font-bold mb-1 ${keyword.color}`}>
        {keyword.name}
      </div>
      <div className="h-px bg-zinc-700 mb-2" />
      <div className="text-xs text-zinc-300 leading-relaxed">
        <RichText text={keyword.description} keywords={keywords} depth={depth} />
      </div>
      {depth >= MAX_DEPTH && (
        <div className="text-[10px] text-zinc-600 mt-1 italic">max depth</div>
      )}
    </div>
  );
}

/** Equipment / Spell — rarity border + smoke bg + icon + stats */
function EquipmentTooltip({ keyword, keywords, depth }: {
  keyword: KeywordDef; keywords: Map<string, KeywordDef>; depth: number;
}) {
  const rarity = keyword.rarity ?? 'common';
  const border = RARITY_BORDER[rarity] ?? RARITY_BORDER.common;

  return (
    <div className={`relative overflow-hidden border rounded-lg shadow-lg bg-zinc-950 ${border}`}>
      {rarity !== 'common' && <SmokeBg rarity={rarity} />}
      <div className="relative z-10 p-3">
        {/* Header */}
        <div className="flex items-center gap-2 mb-1">
          {keyword.icon && <span className="text-lg">{keyword.icon}</span>}
          <div>
            <div className={`text-sm font-bold ${keyword.color}`}>{keyword.name}</div>
            <div className="text-[9px] uppercase tracking-wider text-zinc-500">{rarity}</div>
          </div>
        </div>
        <div className={`h-px mb-2 opacity-30 ${border.replace('border', 'bg')}`} />
        {/* Description */}
        <div className="text-xs text-zinc-300 leading-relaxed">
          <RichText text={keyword.description} keywords={keywords} depth={depth} />
        </div>
        {/* Stats */}
        {keyword.stats && Object.keys(keyword.stats).length > 0 && (
          <div className="mt-2 pt-1.5 border-t border-zinc-800">
            {Object.entries(keyword.stats).map(([k, v]) => (
              <div key={k} className="flex justify-between text-[11px]">
                <span className="text-zinc-500">{k}</span>
                <span className="text-zinc-200">{v}</span>
              </div>
            ))}
          </div>
        )}
      </div>
    </div>
  );
}

/** Status effect — colored left accent bar */
function StatusTooltip({ keyword, keywords, depth }: {
  keyword: KeywordDef; keywords: Map<string, KeywordDef>; depth: number;
}) {
  return (
    <div className="bg-zinc-950 border border-zinc-800 rounded-lg shadow-lg flex overflow-hidden">
      {/* Color accent bar */}
      <div className={`w-1 shrink-0 ${keyword.color.replace('text-', 'bg-')}`} />
      <div className="p-3">
        <div className={`text-sm font-bold mb-1 ${keyword.color}`}>
          {keyword.icon && <span className="mr-1">{keyword.icon}</span>}
          {keyword.name}
        </div>
        <div className="text-xs text-zinc-300 leading-relaxed">
          <RichText text={keyword.description} keywords={keywords} depth={depth} />
        </div>
      </div>
    </div>
  );
}

/**
 * Buff tooltip —— 战斗中角色身上 buff/debuff 的 hover 卡片。
 *
 * 跟 status 的视觉区分:
 *   - status 是"分类术语"(像 wiki 词条):一个色条 + 名字 + 描述
 *   - buff   是"运行时状态实例":名字 + 描述 + **属性修改器列表** + **生命周期信息**
 *
 * 布局:
 *   ┌──────────────────────────────────────┐
 *   │ [icon] 流血 × 3        [Limited: 12s] │  ← header,lifetime badge 右上角
 *   │ ───────────────────────────────────── │
 *   │ 每回合开始时受到 ... 的伤害。           │  ← description
 *   │ ───────────────────────────────────── │
 *   │ 最大生命           ×0.8               │  ← stats(属性修改器)
 *   │ 攻击力             −5                 │
 *   └──────────────────────────────────────┘
 *
 * lifetime badge 的展示规则:
 *   - 'Limited' + remainingRounds >= 0 → 显示"剩余 N 回合"(单位契约:回合,原值直推)
 *   - 'Infinity'                       → 显示"永久"
 *   - 'Immediately' / 未提供            → 不显示(瞬时 buff 一般也不挂在角色身上,UI 不该显示)
 */
function BuffTooltip({ keyword, keywords, depth }: {
  keyword: KeywordDef; keywords: Map<string, KeywordDef>; depth: number;
}) {
  const meta = keyword.buffMeta;
  let lifetimeBadge: string | null = null;
  if (meta?.lifetimeType === 'Limited' && typeof meta.remainingRounds === 'number' && meta.remainingRounds >= 0) {
    // N 已在 C++ 端 eps-ceil 取整(与判死同源),原样显示
    lifetimeBadge = `剩余 ${meta.remainingRounds} 回合`;
  } else if (meta?.lifetimeType === 'Infinity') {
    lifetimeBadge = '永久';
  }

  const modifiers = meta?.modifiers ?? [];

  return (
    <div className="bg-zinc-950 border border-zinc-800 rounded-lg shadow-lg flex overflow-hidden">
      {/* 跟 StatusTooltip 同款左侧色条 —— 视觉上 buff 是 status 的"实例化版本",保持家族感 */}
      <div className={`w-1 shrink-0 ${keyword.color.replace('text-', 'bg-')}`} />
      <div className="p-3 flex-1 min-w-0">
        {/* Header:name + (optional) lifetime badge */}
        <div className="flex items-baseline justify-between gap-2 mb-1">
          <div className={`text-sm font-bold ${keyword.color}`}>
            {keyword.icon && <span className="mr-1">{keyword.icon}</span>}
            {keyword.name}
          </div>
          {lifetimeBadge && (
            <span className="text-[9px] uppercase tracking-wider text-zinc-500 whitespace-nowrap">
              {lifetimeBadge}
            </span>
          )}
        </div>
        <div className={`h-px mb-2 opacity-30 ${keyword.color.replace('text-', 'bg-')}`} />
        {/* Description —— RichText 也走嵌套术语 lookup,buff 描述里可以引用 {Damage.Fire} 之类 */}
        <div className="text-xs text-zinc-300 leading-relaxed">
          <RichText text={keyword.description} keywords={keywords} depth={depth} />
        </div>
        {/* 属性修改器表 —— 按 tone 上色:positive 绿、negative 红、neutral 默认。
            tone 由调用方(BattleUnitStatusPanel.buffToKeywordDef)算好后塞进 buffMeta.modifiers,
            tooltip 这层只看 tone 选 className,不重新推导符号 / 数值大小 → 着色逻辑跟数值
            格式化逻辑保持一处。 */}
        {modifiers.length > 0 && (
          <div className="mt-2 pt-1.5 border-t border-zinc-800 flex flex-col gap-0.5">
            {modifiers.map((m, i) => (
              <div key={i} className="flex justify-between text-[11px]">
                <span className="text-zinc-500">{m.attributeLabel}</span>
                <span
                  className={`tabular-nums ${
                    m.tone === 'positive'
                      ? 'text-emerald-400'
                      : m.tone === 'negative'
                        ? 'text-red-400'
                        : 'text-zinc-200'
                  }`}
                >
                  {m.displayValue}
                </span>
              </div>
            ))}
          </div>
        )}
      </div>
    </div>
  );
}

// ===== PinnedTooltipLayer =====

export function PinnedTooltipLayer() {
  const { pinned, unpinAll } = useContext(PinnedContext);

  useEffect(() => {
    if (pinned.length === 0) return;

    const handler = (e: MouseEvent) => {
      const target = e.target as HTMLElement;
      // Don't dismiss when clicking on pinned windows or keyword spans
      if (target.closest('[data-pinned-tooltip]') || target.closest('[data-keyword-span]')) {
        return;
      }
      unpinAll();
    };

    const id = setTimeout(() => window.addEventListener('mousedown', handler), 0);
    return () => {
      clearTimeout(id);
      window.removeEventListener('mousedown', handler);
    };
  }, [pinned.length, unpinAll]);

  if (pinned.length === 0) return null;

  return (
    <>
      {pinned.map(entry => (
        <PinnedTooltipWindow key={entry.uid} entry={entry} />
      ))}
    </>
  );
}

// ===== PinnedTooltipWindow (draggable) =====

function PinnedTooltipWindow({ entry }: { entry: PinnedEntry }) {
  const { unpin, keywords } = useContext(PinnedContext);
  const [pos, setPos] = useState({ x: entry.x, y: entry.y });
  const [zIndex] = useState(() => nextZIndex++);
  const dragRef = useRef<{ startX: number; startY: number; origX: number; origY: number } | null>(null);

  // Bring to front on mousedown anywhere on the window
  const bringToFront = useCallback(() => {
    // We don't re-render for z-index bump — just set on DOM directly for perf
    // Actually we use a ref-based approach: not needed since each window has its own z
  }, []);

  const onTitleMouseDown = useCallback((e: React.MouseEvent) => {
    e.preventDefault();
    bringToFront();
    const zoom = getZoom();
    dragRef.current = {
      startX: e.clientX,
      startY: e.clientY,
      origX: pos.x,
      origY: pos.y,
    };

    const onMove = (ev: MouseEvent) => {
      if (!dragRef.current) return;
      // Divide delta by zoom — clientX is in physical pixels, left/top are in logical
      setPos({
        x: dragRef.current.origX + (ev.clientX - dragRef.current.startX) / zoom,
        y: dragRef.current.origY + (ev.clientY - dragRef.current.startY) / zoom,
      });
    };

    const onUp = () => {
      dragRef.current = null;
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onUp);
    };

    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);
  }, [pos.x, pos.y, bringToFront]);

  return (
    <div
      data-pinned-tooltip
      className="fixed w-72 animate-[fadeIn_0.15s_ease-out]"
      style={{ left: pos.x, top: pos.y, zIndex }}
    >
      {/* Drag handle + close button overlay */}
      <div className="relative">
        <div
          className="absolute top-0 left-0 right-0 h-7 z-20 flex items-center justify-between px-2
                     cursor-grab active:cursor-grabbing select-none"
          onMouseDown={onTitleMouseDown}
        >
          <span className="text-[10px] text-zinc-500 opacity-60">&#x1F4CC;</span>
          <button
            className="text-zinc-500 hover:text-zinc-200 text-xs px-1"
            onClick={() => unpin(entry.uid)}
          >
            &#x2715;
          </button>
        </div>
        {/* Reuse the same TooltipPanel as hover tooltips */}
        <TooltipPanel keyword={entry.keyword} keywords={keywords} depth={1} />
      </div>
    </div>
  );
}

// ===== ItemCard =====

export function ItemCard({
  item,
  keywords,
  smokeParams,
}: {
  item: ItemDef;
  keywords: Map<string, KeywordDef>;
  smokeParams?: SmokeParams;
}) {
  const rarityStyle = RARITY_STYLES[item.rarity] ?? RARITY_STYLES.common;

  return (
    <div className={`relative overflow-hidden border rounded-lg p-4 w-80 bg-zinc-950 ${rarityStyle.split(' ')[0]}`}>
      {/* Animated smoke background */}
      <SmokeBg rarity={item.rarity} params={smokeParams} />

      {/* Content (above smoke) */}
      <div className="relative z-10">
        <div className="flex items-center gap-2 mb-2">
          <span className="text-2xl">{item.icon}</span>
          <div>
            <div className={`font-bold ${rarityStyle.split(' ').slice(1).join(' ')}`}>
              {item.name}
            </div>
            <div className="text-[10px] uppercase tracking-wider text-zinc-500">
              {item.rarity}
            </div>
          </div>
        </div>
        <div className={`h-px mb-3 opacity-30 ${rarityStyle.split(' ')[0].replace('border', 'bg')}`} />
        <div className="text-sm text-zinc-300 leading-relaxed">
          <RichText text={item.description} keywords={keywords} depth={0} />
        </div>
        {item.stats && Object.keys(item.stats).length > 0 && (
          <div className="mt-3 pt-2 border-t border-zinc-800">
            {Object.entries(item.stats).map(([key, val]) => (
              <div key={key} className="flex justify-between text-xs">
                <span className="text-zinc-500">{key}</span>
                <span className="text-zinc-200">{val}</span>
              </div>
            ))}
          </div>
        )}
      </div>
    </div>
  );
}
