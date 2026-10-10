import { useEffect, useRef, useState } from 'react';
import { AnimatePresence, motion } from 'framer-motion';
import { useBattleStore } from '@/stores/battle-store';
import { useSettingsStore } from '@/stores/settings-store';
import { useTargetingFailureLabel } from '@/i18n/battle-labels';
import { brass, TONE_INK } from '@/config/battle-palette';

/**
 * TargetingHoverTooltip —— 瞄准期"目标不可选中原因"跟随鼠标 tooltip。
 *
 * 数据流:
 *   C++ PC.BattleTargetingState::Tick → raycast cursor + ValidateTarget
 *     → 节流 broadcast Ability.Intent.TargetHovered
 *     → BattleUIDispatcher → CallUI("setTargetingHover", ...)
 *     → store.targetingHover (active/valid/reasonTag/actorName)
 *     → 本组件订阅渲染
 *
 *   独立反馈:玩家点了无效目标 → Ability.Intent.TargetRejected → setTargetingRejectPulse →
 *   store.targetingRejectPulse 自增 trigger → 本组件 useEffect 检测 trigger 变化 → 播一次
 *   shake/flash 动画 → 自调 clearTargetingRejectPulse。
 *
 * # 显示策略
 *
 *   active=false                 → 返 null,完全不渲染(隐藏 tooltip)
 *   active=true && valid=true    → 暂不显示("可选"状态不需要文字干扰;后续可加正向提示)
 *   active=true && valid=false   → 显示原因文案,跟随鼠标
 *
 * # 边缘 clamp 思路
 *
 * 鼠标坐标作为默认 anchor(右下偏移 + MARGIN)。如果右下溢出 → flip 到左上偏移。垂直独立判定
 * (溢出底 → 向上 flip)。这套跟项目 NestedTooltip 的 clampToViewport 同思路,但简化版 —— 这里
 * tooltip 内容短,没有 nested,不需要 anchor rect 复杂判断。
 *
 * # zoom 适配
 *
 * Ultralight 用 CSS zoom 缩放整个 UI,window.innerWidth / clientX 是物理像素,fixed positioning
 * 用逻辑坐标。除以 zoom 得到逻辑坐标。跟 NestedTooltip 一致。
 *
 * # 不订阅 gamepad cursor 的 screen 坐标
 *
 * MVP 只走键鼠路径(window mousemove)。手柄模式下 C++ BattleGamepadCursor 的世界坐标转屏幕坐标
 * 没接进 JS,tooltip 会卡在最后鼠标位置(不严重,因为手柄玩家本来就用 cursor pawn 视觉锁敌)。
 * 后续如要修:加一条 UE→JS 推送 cursor 屏幕坐标,组件再读那个 store。
 */

const OFFSET_X = 18;
const OFFSET_Y = 18;
const MARGIN = 12;
const REJECT_ANIM_DURATION_MS = 600;

function getZoom(): number {
  const z = parseFloat(document.documentElement.style.zoom ?? '1');
  return Number.isFinite(z) && z > 0 ? z : 1;
}

export function TargetingHoverTooltip()
{
  const hover = useBattleStore((s) => s.targetingHover);
  const rejectPulse = useBattleStore((s) => s.targetingRejectPulse);
  const clearRejectPulse = useBattleStore((s) => s.clearTargetingRejectPulse);
  const uiScale = useSettingsStore((s) => s.uiScale);
  const getFailureLabel = useTargetingFailureLabel();

  // 鼠标位置(逻辑坐标)。useRef 不参与 re-render,window mousemove handler 直接写,渲染期
  // 读自 ref 算 anchor。换 useState 会每次 mousemove 都重渲整个组件 + framer-motion 重算
  // transform,在 Ultralight 上明显丢帧。
  const [mousePos, setMousePos] = useState({ x: 0, y: 0 });

  // 监听 window mousemove,把物理像素 / zoom 算成逻辑像素存进 state。零节流 —— 移动鼠标本来
  // 就低频(< 60Hz),React state 更新自带 batching。
  useEffect(() =>
  {
    const handler = (e: MouseEvent) =>
    {
      const z = getZoom();
      setMousePos({ x: e.clientX / z, y: e.clientY / z });
    };
    window.addEventListener('mousemove', handler);
    return () => window.removeEventListener('mousemove', handler);
  }, []);

  // Reject pulse 检测:trigger 变化时启动一次性 shake 动画,duration 到了清 store。
  const [pulseKey, setPulseKey] = useState(0);
  const lastPulseTriggerRef = useRef<number>(0);

  useEffect(() =>
  {
    if (!rejectPulse)
    {
      // store 清空(clearTargetingRejectPulse)后 trigger 计数会从 1 重新开始
      // (push 是 `(state.targetingRejectPulse?.trigger ?? 0) + 1`)。这里必须把
      // ref 同步归零,否则下一次 pulse 的 trigger(=1)跟旧 ref(=1)相等 → 早退,
      // 既不重播 shake 也不调度 clear,tooltip 永久卡在 reject 红色态。
      lastPulseTriggerRef.current = 0;
      return;
    }
    if (rejectPulse.trigger === lastPulseTriggerRef.current)
    {
      return;
    }
    lastPulseTriggerRef.current = rejectPulse.trigger;
    setPulseKey((k) => k + 1);  // 让 motion.div 重 mount 触发 enter animation

    const handle = setTimeout(() => clearRejectPulse(), REJECT_ANIM_DURATION_MS);
    return () => clearTimeout(handle);
  }, [rejectPulse?.trigger, clearRejectPulse, rejectPulse]);

  // 只在 active && !valid 时显示(`!valid` 包括了 reject pulse 的场景,因为 reject 触发时通常
  // hover state 也是 invalid 的)。如果想正向 "可选" 提示,这里加分支。
  const shouldShow = hover.active && !hover.valid;

  // 测量 tooltip 大小做 edge clamp。getBoundingClientRect 在 first render 不可用,用 ref + state
  // 在 mount 后回填一次;之后 size 基本不变(文案改变会 trigger 重算)。
  const tooltipRef = useRef<HTMLDivElement>(null);
  const [tooltipSize, setTooltipSize] = useState({ w: 0, h: 0 });

  useEffect(() =>
  {
    if (!tooltipRef.current || !shouldShow)
    {
      return;
    }
    const rect = tooltipRef.current.getBoundingClientRect();
    const z = getZoom();
    setTooltipSize({ w: rect.width / z, h: rect.height / z });
  }, [shouldShow, hover.reasonTag, hover.actorName, uiScale]);

  // Edge clamp:默认 anchor 在鼠标右下;若右溢出 flip 到左侧,若下溢出 flip 到上方。
  const z = getZoom();
  const vw = window.innerWidth / z;
  const vh = window.innerHeight / z;

  let anchorX = mousePos.x + OFFSET_X;
  let anchorY = mousePos.y + OFFSET_Y;

  if (tooltipSize.w > 0 && anchorX + tooltipSize.w > vw - MARGIN)
  {
    anchorX = mousePos.x - OFFSET_X - tooltipSize.w;
  }
  if (tooltipSize.h > 0 && anchorY + tooltipSize.h > vh - MARGIN)
  {
    anchorY = mousePos.y - OFFSET_Y - tooltipSize.h;
  }
  // 兜底:即使翻转后仍然贴边外(极端小屏 + 大文字)就 clamp 到 MARGIN 内,信息不丢。
  anchorX = Math.max(MARGIN, Math.min(anchorX, vw - tooltipSize.w - MARGIN));
  anchorY = Math.max(MARGIN, Math.min(anchorY, vh - tooltipSize.h - MARGIN));

  const reasonLabel = hover.reasonTag ? getFailureLabel(hover.reasonTag) : '';
  // Reject 闪烁期 —— 600ms 后 clearRejectPulse 把 store 清空,样式自动回到普通 hover 态
  const isReject = pulseKey > 0 && rejectPulse != null;

  return (
    <AnimatePresence>
      {shouldShow && (
        <motion.div
          ref={tooltipRef}
          // pulseKey 在 reject 时变化让 motion.div 重 mount → 播 enter animation(shake)。
          // 平时(纯 hover)pulseKey 不变,只 update 位置 / 文案,不 replay。
          key={`tooltip-${pulseKey}`}
          initial={{ opacity: 0, scale: 0.9 }}
          animate={
            isReject
              ? {
                  // Reject 路径:shake + flash red
                  opacity: 1,
                  scale: [1, 1.08, 1],
                  x: [0, -6, 6, -4, 4, 0],
                  transition: { duration: REJECT_ANIM_DURATION_MS / 1000, ease: 'easeOut' },
                }
              : { opacity: 1, scale: 1 }
          }
          exit={{ opacity: 0, scale: 0.9, transition: { duration: 0.15 } }}
          className="fixed pointer-events-none select-none z-50"
          // 无框"铭刻"风格(跟浮空血条铭牌同语言):没有实体面板盒,只有羽化椭圆暗色
          // 衬底保证世界之上的可读性。原因文字走石榴红 ink(tooltip 只在"不可选中"时
          // 出现,语义即负面);reject 闪烁提亮 ink + 衬底染石榴红,配合 shake。
          style={{
            left: `${anchorX}px`,
            top: `${anchorY}px`,
            fontSize: '0.85em',
            fontFamily: 'var(--font-display)',
            color: isReject ? '#f0cac4' : TONE_INK.negative,
            // 同浮空血条铭牌的双层 text-shadow —— 紧致暗色剪影,不是光晕
            textShadow: '0 1px 2px rgba(0,0,0,0.9), 0 0 2px rgba(0,0,0,0.7)',
            // 行宽限制 + word-wrap 防止单条原因文案过长撑爆屏幕。max-width 取屏幕的 1/4 当上限。
            maxWidth: `${Math.min(360, vw * 0.25)}px`,
            whiteSpace: 'normal',
            lineHeight: 1.3,
            display: 'flex',
            flexDirection: 'column',
            alignItems: 'center',
            textAlign: 'center',
          }}
        >
          {/* 羽化暗色衬底 —— 配方同浮空血条铭牌(见 HealthBar.tsx 注释)。reject 期
              整片衬底染深石榴红,比边框闪红更"浸入"也更克制。 */}
          <span
            aria-hidden
            className="absolute pointer-events-none"
            style={{
              inset: '-8px -26px',
              background: isReject
                ? 'radial-gradient(ellipse at center, rgba(64,18,12,0.72) 0%, rgba(64,18,12,0.52) 34%, rgba(64,18,12,0.24) 56%, rgba(64,18,12,0) 72%)'
                : 'radial-gradient(ellipse at center, rgba(0,0,0,0.66) 0%, rgba(0,0,0,0.5) 34%, rgba(0,0,0,0.22) 56%, rgba(0,0,0,0) 72%)',
            }}
          />
          {/* 目标名 —— 两侧黄铜渐隐细线(── 名字 ──),跟浮空血条铭牌完全同款 */}
          {hover.actorName && (
            <div
              className="relative flex items-center"
              style={{ fontSize: '0.72em', color: '#d4c4a0', fontWeight: 600, letterSpacing: '0.08em' }}
            >
              <span
                aria-hidden
                style={{ width: 10, height: 1, marginRight: 5, background: `linear-gradient(90deg, ${brass(0)}, ${brass(0.6)})` }}
              />
              <span>{hover.actorName}</span>
              <span
                aria-hidden
                style={{ width: 10, height: 1, marginLeft: 5, background: `linear-gradient(270deg, ${brass(0)}, ${brass(0.6)})` }}
              />
            </div>
          )}
          <div className="relative" style={{ fontWeight: 600, fontSize: '1em', marginTop: hover.actorName ? '0.1em' : 0 }}>
            {reasonLabel || '无法选中'}
          </div>
        </motion.div>
      )}
    </AnimatePresence>
  );
}
