import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  useSyncExternalStore,
  type ReactNode,
} from 'react';
import { createPortal } from 'react-dom';
import { isInUnreal } from '@/lib/ue-bridge';
import { useLogicalViewport } from '@/lib/use-logical-viewport';
import { usePageActive } from '@/components/PageActivity';

/**
 * 嵌套 tooltip 系统（自 CRPGFramework 的 NestedTooltip 移植裁剪）。
 *
 *   <Hoverable content={...}>值 / 术语 / 链接</Hoverable>
 *
 * 悬停出卡片（portal 到 body，fixed 定位，贴边翻转），卡片本身可再悬停 —— 深度由
 * DepthContext 传递，最多 3 层。点击或悬停一秒可钉成可拖拽窗口（Esc / ✕ 关闭）。
 *
 * 坐标约定（Ultralight / WebKit 615 实测，与 CRPGFramework 的 NestedTooltip 一致）：
 * innerWidth / clientX 是视觉（物理）像素，getBoundingClientRect 是布局（逻辑）像素，
 * fixed 定位用逻辑坐标 —— 只有视口尺寸与鼠标位移需要除以 html zoom，rect 直接用。
 * body / #root 默认 pointer-events:none，卡片自己开 pointer-events-auto 才接得到悬停。
 */

const MAX_DEPTH = 3;
const HIDE_DELAY_MS = 150;
const HOVER_PIN_MS = 1000;
const MARGIN = 12;
const CARD_W = 300;
const CARD_H_EST = 140;
/** Progress arc of the pin ring (radius 7 around 10,10), clockwise from the top. */
function pinRingArc(progress: number): string {
  const angle = Math.min(.9999, Math.max(0, progress)) * 2 * Math.PI;
  const x = 10 + 7 * Math.sin(angle), y = 10 - 7 * Math.cos(angle);
  return `M 10 3 A 7 7 0 ${angle > Math.PI ? 1 : 0} 1 ${x.toFixed(2)} ${y.toFixed(2)}`;
}
// 在首次加载时检测真实宿主；热更新也保留结果，避免把后装的浏览器 mock 误判为 Ultralight。
const ULTRALIGHT_COORDINATES: boolean = import.meta.hot?.data.ultralightCoordinates ?? isInUnreal();
if (import.meta.hot) import.meta.hot.data.ultralightCoordinates = ULTRALIGHT_COORDINATES;

export function getZoom(): number {
  const z = parseFloat(document.documentElement.style.zoom || '1');
  return Number.isFinite(z) && z > 0 ? z : 1;
}

/** 现代浏览器的 rect 是视觉像素，Ultralight 615 的 rect 已是逻辑像素。 */
export function getLayoutRect(rect: DOMRect): DOMRect {
  const scale = ULTRALIGHT_COORDINATES ? 1 : getZoom();
  return new DOMRect(rect.x / scale, rect.y / scale, rect.width / scale, rect.height / scale);
}

/** 锚点右侧优先，溢出则翻到左侧；垂直贴边钳制。rect 已是逻辑坐标，只有视口要除 zoom。 */
function placeNearAnchor(rect: DOMRect, w: number, h: number): { x: number; y: number } {
  const z = getZoom();
  const vw = window.innerWidth / z;
  const vh = window.innerHeight / z;
  const layout = getLayoutRect(rect);
  const left = layout.left;
  const right = layout.right;
  const top = layout.top;
  let x = right + 8;
  let y = top - 4;
  if (x + w > vw - MARGIN) x = left - w - 8;
  if (x < MARGIN) x = MARGIN;
  if (y + h > vh - MARGIN) y = vh - h - MARGIN;
  if (y < MARGIN) y = MARGIN;
  return { x, y };
}

interface PinnedEntry {
  width?: number;
  uid: number;
  content: ReactNode;
  x: number;
  y: number;
}

interface TooltipContextValue {
  pin: (content: ReactNode, x: number, y: number, width?: number) => number;
  isPinned: (uid: number) => boolean;
  unpin: (uid: number) => void;
  unpinAll: () => void;
}

const TooltipContext = createContext<TooltipContextValue>({
  pin: () => 0,
  isPinned: () => false,
  unpin: () => {},
  unpinAll: () => {},
});
const DepthContext = createContext(0);
const LayerContext = createContext(1000);
let nextUid = 1;
let nextZ = 1200;

/** 命令式入口：面板退场时可 unpinAll 收掉自己钉出的窗口。 */
// eslint-disable-next-line react-refresh/only-export-components
export function useTooltips() {
  return useContext(TooltipContext);
}

export function TooltipProvider({ children }: { children: ReactNode }) {
  const active = usePageActive();
  const [pinned, setPinned] = useState<PinnedEntry[]>([]);
  const pinnedIds = useRef(new Set<number>());
  const pin = useCallback((content: ReactNode, x: number, y: number, width = CARD_W) => {
    const uid = nextUid++;
    pinnedIds.current.add(uid);
    setPinned((prev) => [...prev, { uid, content, x, y, width }]);
    return uid;
  }, []);
  const isPinned = useCallback((uid: number) => pinnedIds.current.has(uid), []);
  const unpin = useCallback((uid: number) => {
    pinnedIds.current.delete(uid);
    setPinned((prev) => prev.filter((e) => e.uid !== uid));
  }, []);
  const unpinAll = useCallback(() => {
    pinnedIds.current.clear();
    setPinned([]);
  }, []);

  /** 钉住卡片保留供对照查阅，直到 Esc / ✕；继续操作原面板不会把它关闭。 */
  useEffect(() => {
    if (!active || pinned.length === 0) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && !e.defaultPrevented && !document.querySelector('[data-select-menu]')) {
        e.preventDefault();
        unpinAll();
      }
    };
    const id = setTimeout(() => {
      window.addEventListener('keydown', onKey);
    }, 0);
    return () => {
      clearTimeout(id);
      window.removeEventListener('keydown', onKey);
    };
  }, [active, pinned.length, unpinAll]);

  return (
    <TooltipContext.Provider value={{ pin, isPinned, unpin, unpinAll }}>
      {children}
      {pinned.map((entry) => (
        <PinnedWindow key={entry.uid} entry={entry} onClose={() => unpin(entry.uid)} />
      ))}
    </TooltipContext.Provider>
  );
}

/** 文字滚动层；笔触轮廓由外侧容器绘制，不随内容裁切。 */
export function TooltipFrame({ children }: { children: ReactNode }) {
  return <div className="strategy-tooltip-frame">{children}</div>;
}

interface HoverableProps {
  /** 操作已展开时可暂时关闭摘要，避免悬浮卡遮住正在使用的内容。 */
  enabled?: boolean;
  /** SVG charts use a group anchor; tooltip content still renders in the HTML portal. */
  as?: 'span' | 'g';
  ariaLabel?: string;
  width?: number;
  /** 卡片内容（可含 Hoverable，自动嵌套计深）。 */
  content: ReactNode;
  children: ReactNode;
  className?: string;
  /** 点击锚点是否钉住卡片；链接类锚点自己处理点击时传 false。 */
  pinnable?: boolean;
  /** 默认跟随 pinnable；跳转和操作类提示不启用。 */
  pinOnHover?: boolean;
}

interface LiveSource {
  content: ReactNode;
  listeners: Set<() => void>;
}
/** 钉住后继续订阅原锚点的快照更新；锚点离场后保留最后一次有效内容。 */
function LiveTooltipContent({ source }: { source: LiveSource }) {
  const subscribe = useCallback(
    (listener: () => void) => {
      source.listeners.add(listener);
      return () => {
        source.listeners.delete(listener);
      };
    },
    [source],
  );
  return useSyncExternalStore(subscribe, () => source.content);
}

export function Hoverable({
  enabled = true,
  as: Tag = 'span',
  ariaLabel,
  content,
  children,
  className = '',
  width = CARD_W,
  pinnable = true,
  pinOnHover = pinnable,
}: HoverableProps) {
  const pageActive = usePageActive();
  const active = pageActive && enabled;
  const depth = useContext(DepthContext);
  const { pin, isPinned } = useContext(TooltipContext);
  const [open, setOpen] = useState(false);
  const [pinProgress, setPinProgress] = useState(0);
  const hideTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const pinTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const progressTimer = useRef<ReturnType<typeof setInterval> | null>(null);
  const pinnedUid = useRef(0);
  const anchorRef = useRef<HTMLSpanElement | SVGGElement>(null);
  const cardRef = useRef<HTMLDivElement>(null);
  const canNest = depth < MAX_DEPTH;
  const [source] = useState<LiveSource>(() => ({ content, listeners: new Set() }));
  useLayoutEffect(() => {
    source.content = content;
    source.listeners.forEach((listener) => listener());
  }, [content, source]);

  const clearPinTimers = useCallback(() => {
    if (pinTimer.current !== null) clearTimeout(pinTimer.current);
    if (progressTimer.current !== null) clearInterval(progressTimer.current);
    pinTimer.current = null;
    progressTimer.current = null;
  }, []);
  const cancelPin = useCallback(() => {
    clearPinTimers();
    setPinProgress(0);
  }, [clearPinTimers]);
  const pinCurrent = useCallback(() => {
    cancelPin();
    if (hideTimer.current !== null) clearTimeout(hideTimer.current);
    if (!active || !anchorRef.current || isPinned(pinnedUid.current)) return;
    // 复用正在阅读的卡片位置，自动钉住时不会跳到另一个角落。
    const rect = cardRef.current && getLayoutRect(cardRef.current.getBoundingClientRect());
    const pos = rect
      ? { x: rect.x, y: rect.y }
      : placeNearAnchor(anchorRef.current.getBoundingClientRect(), width, CARD_H_EST);
    pinnedUid.current = pin(<LiveTooltipContent source={source} />, pos.x, pos.y, width);
    setOpen(false);
  }, [active, cancelPin, isPinned, pin, source, width]);
  const startPin = useCallback(() => {
    if (!active || !pinOnHover || !canNest || pinTimer.current !== null || isPinned(pinnedUid.current)) return;
    const started = performance.now();
    // 数据快照更新不重新启动计时。只有正在悬停的卡片以 20 Hz 更新右上角进度环。
    progressTimer.current = setInterval(
      () => setPinProgress(Math.min(1, (performance.now() - started) / HOVER_PIN_MS)),
      50,
    );
    pinTimer.current = setTimeout(pinCurrent, HOVER_PIN_MS);
  }, [active, canNest, isPinned, pinCurrent, pinOnHover]);
  const show = useCallback(() => {
    if (!active) return;
    if (hideTimer.current) {
      clearTimeout(hideTimer.current);
      hideTimer.current = null;
    }
    if (!isPinned(pinnedUid.current)) setOpen(true);
  }, [active, isPinned]);
  const hide = useCallback(() => {
    cancelPin();
    if (hideTimer.current) clearTimeout(hideTimer.current);
    hideTimer.current = setTimeout(() => setOpen(false), HIDE_DELAY_MS);
  }, [cancelPin]);
  useEffect(() => {
    window.addEventListener('blur', hide);
    return () => {
      if (hideTimer.current) clearTimeout(hideTimer.current);
      clearPinTimers();
      window.removeEventListener('blur', hide);
    };
  }, [clearPinTimers, hide]);

  useLayoutEffect(() => {
    if (active) return;
    cancelPin();
    if (hideTimer.current !== null) clearTimeout(hideTimer.current);
    setOpen(false);
  }, [active, cancelPin]);

  const onMouseOver = (event: React.MouseEvent<Element>) => {
    const target = event.target as Element;
    // React portal 事件也向锚点冒泡；只给鼠标实际所在的这一层计时。
    if (!event.currentTarget.contains(target)) return;
    const nestedAnchor = target.closest('[data-tooltip-anchor]');
    if (nestedAnchor && nestedAnchor !== anchorRef.current) cancelPin();
    else startPin();
  };
  const onMouseOut = (event: React.MouseEvent<Element>) => {
    if (!event.currentTarget.contains(event.target as Node)) return;
    const next = event.relatedTarget;
    if (!(next instanceof Node) || !event.currentTarget.contains(next)) cancelPin();
  };

  const onClick = (e: React.MouseEvent) => {
    if (!pinnable || !anchorRef.current?.contains(e.target as Node)) return;
    if ((e.target as Element).closest('[data-tooltip-anchor]') !== anchorRef.current) return;
    e.stopPropagation();
    pinCurrent();
  };

  return (
    <Tag
      ref={(node: HTMLSpanElement | SVGGElement | null) => { anchorRef.current = node; }}
      data-tooltip-anchor
      aria-label={ariaLabel}
      tabIndex={pinnable ? 0 : undefined}
      role={pinnable ? 'button' : undefined}
      className={`pointer-events-auto ${className}`}
      onMouseEnter={canNest ? show : undefined}
      onMouseLeave={canNest ? hide : undefined}
      onMouseOver={canNest ? onMouseOver : undefined}
      onMouseOut={canNest ? onMouseOut : undefined}
      onFocus={canNest ? show : undefined}
      onBlur={canNest ? hide : undefined}
      onKeyDown={(event) => {
        if (pinnable && event.target === event.currentTarget && (event.key === 'Enter' || event.key === ' ')) {
          event.preventDefault();
          event.stopPropagation();
          pinCurrent();
        }
      }}
      onClick={onClick}
    >
      {children}
      {active && open && canNest ? (
        <HoverCard
          width={width}
          anchorRef={anchorRef}
          cardRef={cardRef}
          depth={depth + 1}
          onMouseEnter={show}
          onMouseLeave={hide}
          onMouseOver={onMouseOver}
          onMouseOut={onMouseOut}
          pinProgress={pinOnHover ? pinProgress : undefined}
        >
          {content}
        </HoverCard>
      ) : null}
    </Tag>
  );
}

function HoverCard({
  width,
  anchorRef,
  cardRef,
  depth,
  children,
  onMouseEnter,
  onMouseLeave,
  onMouseOver,
  onMouseOut,
  pinProgress,
}: {
  width: number;
  anchorRef: React.RefObject<HTMLSpanElement | SVGGElement | null>;
  cardRef: React.RefObject<HTMLDivElement | null>;
  depth: number;
  children: ReactNode;
  onMouseEnter: () => void;
  onMouseLeave: () => void;
  onMouseOver: (event: React.MouseEvent<Element>) => void;
  onMouseOut: (event: React.MouseEvent<Element>) => void;
  pinProgress?: number;
}) {
  const [pos, setPos] = useState<{ x: number; y: number } | null>(null);
  const viewport = useLogicalViewport();
  const layer = useContext(LayerContext) + 10;
  useLayoutEffect(() => {
    const place = () => {
      if (!anchorRef.current || !cardRef.current) return;
      const card = getLayoutRect(cardRef.current.getBoundingClientRect());
      const next = placeNearAnchor(anchorRef.current.getBoundingClientRect(), card.width, card.height);
      setPos((prev) => (prev?.x === next.x && prev.y === next.y ? prev : next));
    };
    place();
    const observer = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(place);
    if (cardRef.current) observer?.observe(cardRef.current);
    window.addEventListener('resize', place);
    window.addEventListener('scroll', place, true);
    return () => {
      observer?.disconnect();
      window.removeEventListener('resize', place);
      window.removeEventListener('scroll', place, true);
    };
  }, [anchorRef, cardRef, children, viewport.width, viewport.height]);

  return createPortal(
    <div
      ref={cardRef}
      data-hover-tooltip
      className="pointer-events-auto fixed w-[300px] cursor-help animate-[fadeIn_.15s_ease-out]"
      style={{
        width,
        left: pos?.x ?? 0,
        top: pos?.y ?? 0,
        visibility: pos ? 'visible' : 'hidden',
        zIndex: layer,
        maxWidth: viewport.width - 24,
        maxHeight: viewport.height - 24,
        overflowY: 'auto',
      }}
      onMouseEnter={onMouseEnter}
      onMouseLeave={onMouseLeave}
      onMouseOver={onMouseOver}
      onMouseOut={onMouseOut}
    >
      <LayerContext.Provider value={layer}>
        <DepthContext.Provider value={depth}>
          <div className={`relative ${pinProgress !== undefined ? 'tooltip-with-pin-control' : ''}`}>
            {pinProgress !== undefined && (
              <div
                className="tooltip-corner-control tooltip-pin-ring"
                role="progressbar"
                aria-label="悬停一秒钉住"
                aria-valuemin={0}
                aria-valuemax={100}
                aria-valuenow={Math.round(pinProgress * 100)}
              >
                <svg viewBox="0 0 20 20" fill="none" stroke="currentColor" strokeWidth="1.5" aria-hidden="true">
                  <circle cx="10" cy="10" r="7" opacity=".25" />
                  {/* An arc path, not a dash offset: Ultralight would keep the dash pattern for every later SVG stroke. */}
                  {pinProgress > 0 && <path d={pinRingArc(pinProgress)} strokeLinecap="round" />}
                </svg>
              </div>
            )}
            <TooltipFrame>{children}</TooltipFrame>
          </div>
        </DepthContext.Provider>
      </LayerContext.Provider>
    </div>,
    document.body,
  );
}

/** 钉住的窗口：标题条拖拽、✕ 关闭；内容与悬停卡同源。 */
function PinnedWindow({ entry, onClose }: { entry: PinnedEntry; onClose: () => void }) {
  const active = usePageActive();
  const [pos, setPos] = useState({ x: entry.x, y: entry.y });
  const [zIndex] = useState(() => nextZ++);
  const frameRef = useRef<HTMLDivElement>(null);
  const viewport = useLogicalViewport();
  const dragCleanup = useRef<(() => void) | null>(null);
  const drag = useRef<{ startX: number; startY: number; origX: number; origY: number } | null>(null);

  useLayoutEffect(() => {
    if (active) return;
    dragCleanup.current?.();
    const focused = document.activeElement;
    if (focused instanceof HTMLElement && frameRef.current?.contains(focused)) focused.blur();
  }, [active]);

  useLayoutEffect(() => {
    const clamp = () => {
      if (!frameRef.current) return;
      const rect = getLayoutRect(frameRef.current.getBoundingClientRect());
      const maxX = Math.max(MARGIN, window.innerWidth / getZoom() - rect.width - MARGIN);
      const maxY = Math.max(MARGIN, window.innerHeight / getZoom() - rect.height - MARGIN);
      setPos((p) => {
        const x = Math.max(MARGIN, Math.min(p.x, maxX));
        const y = Math.max(MARGIN, Math.min(p.y, maxY));
        return x === p.x && y === p.y ? p : { x, y };
      });
    };
    clamp();
    const observer = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(clamp);
    if (frameRef.current) observer?.observe(frameRef.current);
    window.addEventListener('resize', clamp);
    return () => {
      observer?.disconnect();
      window.removeEventListener('resize', clamp);
      dragCleanup.current?.();
    };
  }, [viewport.width, viewport.height]);

  const onTitleMouseDown = (e: React.MouseEvent) => {
    if (e.button !== 0 || (e.target as HTMLElement).closest('button')) return;
    e.preventDefault();
    const z = getZoom();
    drag.current = { startX: e.clientX, startY: e.clientY, origX: pos.x, origY: pos.y };
    const onMove = (ev: MouseEvent) => {
      if (!drag.current) return;
      setPos({
        x: Math.max(
          MARGIN,
          Math.min(window.innerWidth / z - 60, drag.current.origX + (ev.clientX - drag.current.startX) / z),
        ),
        y: Math.max(
          MARGIN,
          Math.min(window.innerHeight / z - 30, drag.current.origY + (ev.clientY - drag.current.startY) / z),
        ),
      });
    };
    const onUp = () => {
      drag.current = null;
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onUp);
      dragCleanup.current = null;
    };
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);
    dragCleanup.current = onUp;
  };

  return createPortal(
    <div
      ref={frameRef}
      data-pinned-tooltip
      aria-hidden={!active}
      className={`pointer-events-auto fixed w-[300px] cursor-help animate-[fadeIn_.15s_ease-out] ${active ? '' : 'page-hidden'}`}
      style={{
        width: entry.width ?? CARD_W,
        left: pos.x,
        top: pos.y,
        zIndex,
        maxWidth: viewport.width - 24,
        maxHeight: viewport.height - 24,
        overflowY: 'auto',
      }}
    >
      <div className="relative tooltip-with-pin-control">
        <div
          className="absolute inset-x-0 top-0 z-20 flex h-7 cursor-grab select-none items-center pl-2 pr-8 active:cursor-grabbing"
          onMouseDown={onTitleMouseDown}
        >
          <span className="text-[10px] tracking-widest text-[#a99771]">◇ 已钉住 · 拖动移位</span>
          <button
            type="button"
            aria-label="关闭钉住的提示"
            className="tooltip-corner-control cursor-pointer text-[11px] text-[#8794a1] hover:text-[#dde4ec]"
            onClick={onClose}
          >
            ✕
          </button>
        </div>
        <LayerContext.Provider value={zIndex + 10}>
          <DepthContext.Provider value={1}>
            <TooltipFrame>{entry.content}</TooltipFrame>
          </DepthContext.Provider>
        </LayerContext.Provider>
      </div>
    </div>,
    document.body,
  );
}
