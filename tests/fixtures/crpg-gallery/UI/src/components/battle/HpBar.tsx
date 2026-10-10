/**
 * HpBar —— 战斗 UI 的血条组件。
 *
 * 视觉语言:"雕版黄铜仪表"(antique engraved gauge)—— 跟浮空血条
 * (floating-ui/HealthBar.tsx)共用 config/battle-palette 的三档油画色阶
 * (月桂绿 / 古金赭 / 石榴红)+ 黄铜细线框,两处 HP 读数读作同一件仪器。
 * 维多利亚新古典约束:细线、平面、无光晕。
 *
 * 视觉层次(由内到外):
 *   1. 槽:不透明深底 + 黄铜 1px 细线框 + 内圈 1px 暗色雕刻槽(0 blur,平面)
 *   2. 填充:三档油画渐变,顶部 1px catch-light(手绘高光线,不是光泽管)
 *   3. 伤害余像:HP 下降时旧值→新值的暖色残条停顿一拍后收缩(跟浮空血条同款)
 *   4. 治疗扫光:HP 上升时浅羊皮色斜带扫过一次(event-driven,平时安静 ——
 *      早期常驻循环版玩家反馈"一直闪很分心",保持事件驱动语义)
 *   5. 刻度:25/50/75 三条雕刻细线(古董仪表刻度,跟浮空血条同款)
 *   6. Critical:HP < 30% 时框内石榴红细环呼吸(收敛,无外发光)
 *
 * 性能(沿用之前验证过的取舍):
 *   - 填充动 scaleX 而非 width —— width 是 layout 属性每帧触发重排 + 重绘,
 *     Ultralight 小帧预算下肉眼掉帧;scaleX 走 GPU 合成(origin 左对齐)。
 *     顶部 catch-light 是水平线,横向缩放不改变观感;刻度线是槽的兄弟层,
 *     不受填充缩放影响。
 *   - **不**手动设 will-change —— framer-motion 在 scaleX 动画期间自动加并在
 *     结束后移除;永久 will-change 会让 Ultralight 1.4 把 fill 当永久合成层
 *     每帧重处理 → WebUI 永不 idle。
 *   - 扫光走 CSS keyframe + `key={shimmerToken}` remount 重播 —— 一次性动画
 *     不为它起 framer RAF loop。
 *   - Critical 呼吸只动 opacity(GPU 合成 alpha,基本免费);之前 Heart icon 的
 *     blur halo 已移除 —— 跟"无光晕"的版画语言冲突,scale 心跳足够传达紧迫。
 */

import { motion, AnimatePresence } from 'framer-motion';
import { Heart } from 'lucide-react';
import { useState } from 'react';
import { BAR_GROUND, brass, garnet, hpTierPalette } from '@/config/battle-palette';

interface HpBarProps {
  current: number;
  max: number;
}

/** 25/50/75 三条仪表刻度 —— 跟浮空血条(HealthBar.tsx)同款。 */
const TICKS = [25, 50, 75] as const;

/** 整数不带小数点;非整数保留 1 位。HP 通常是整数,但有些 buff 可能产生小数。 */
function formatHp(n: number): string {
  return Number.isInteger(n) ? n.toString() : n.toFixed(1);
}

export function HpBar({ current, max }: HpBarProps) {
  const pct = max > 0 ? Math.max(0, Math.min(1, current / max)) : 0;
  const isCritical = pct > 0 && pct < 0.3;
  const palette = hpTierPalette(pct);

  // 前值对比 —— render 阶段 setState(React 官方推荐的"对比前一次渲染"模式,
  // useState 文档 § "Storing information from previous renders";useEffect 里
  // setState 会被 react-hooks/set-state-in-effect 拒,useRef 在 render 读又被
  // react-hooks/refs 拒)。变化方向决定反馈形式:
  //   下降 → trailPct 记住旧 pct,渲染一条从旧值收缩到新值的暖色"余像"(伤害幅度可视)
  //   上升 → shimmerToken+1,触发一次浅色扫光(治疗确认)
  const [previousPct, setPreviousPct] = useState(pct);
  const [trailPct, setTrailPct] = useState(pct);
  const [shimmerToken, setShimmerToken] = useState(0);
  if (previousPct !== pct) {
    if (pct > previousPct) setShimmerToken((t) => t + 1);
    setTrailPct(previousPct);
    setPreviousPct(pct);
  }
  const showTrail = trailPct > pct;

  return (
    <div className="flex items-center gap-2">
      {/* 左侧 Heart icon —— 染当前档位的油画 ink 色,濒危时 scale 心跳 */}
      <motion.div
        className="shrink-0 flex items-center justify-center"
        animate={isCritical ? { scale: [1, 1.12, 1] } : { scale: 1 }}
        transition={{ duration: 1.4, repeat: isCritical ? Infinity : 0, ease: 'easeInOut' }}
      >
        <Heart
          className="size-4"
          fill="currentColor"
          strokeWidth={1.5}
          style={{ color: palette.ink }}
        />
      </motion.div>

      {/* Bar 主体 —— flex-1 占满剩余宽度 */}
      <div className="flex-1 flex flex-col gap-0.5">
        {/* 顶部数字栏 —— 左侧标签 + 右侧 current/max。数值平时羊皮纸色保持安静,
            只在濒危时染石榴红 ink(古典克制:不给健康态上绿数字)。 */}
        <div className="flex items-baseline justify-between text-[10px] leading-none">
          <span className="text-parchment-dark/70 uppercase tracking-[0.2em]">Vitality</span>
          <span className="font-[family-name:var(--font-subtitle)] tabular-nums">
            <span
              className={isCritical ? 'font-semibold' : 'text-parchment'}
              style={isCritical ? { color: palette.ink } : undefined}
            >
              {formatHp(current)}
            </span>
            <span className="text-parchment-dark/60 mx-1">/</span>
            <span className="text-parchment-dark">{formatHp(max)}</span>
          </span>
        </div>

        {/* 槽:黄铜细线框 + 不透明深底 + 内圈 1px 暗色雕刻槽 */}
        <div
          className="relative h-2.5 overflow-hidden"
          style={{
            background: BAR_GROUND,
            border: `1px solid ${brass(0.55)}`,
            borderRadius: 2,
            boxShadow: 'inset 0 0 0 1px rgba(8,6,3,0.6)',
          }}
        >
          {/* 伤害余像 —— 旧值停顿一拍后收缩到新值,同时淡出。key 在每次变化时换,
              remount 重新起动画(同浮空血条的 trail 节奏/配色) */}
          {showTrail && (
            <motion.div
              key={`${trailPct}-${pct}`}
              className="absolute inset-px origin-left pointer-events-none"
              initial={{ scaleX: trailPct, opacity: 0.85 }}
              animate={{ scaleX: pct, opacity: 0 }}
              transition={{
                scaleX: { duration: 0.55, delay: 0.1, ease: 'easeOut' },
                opacity: { duration: 0.3, delay: 0.35, ease: 'easeOut' },
              }}
              style={{ background: 'linear-gradient(90deg, rgba(221,192,121,0.5), rgba(192,114,106,0.62))' }}
              aria-hidden
            />
          )}

          {/* 填充 —— 满宽 + scaleX(pct)(性能取舍见文件头注释) */}
          <motion.div
            className="absolute inset-px origin-left overflow-hidden"
            initial={false}
            animate={{ scaleX: pct }}
            transition={{ duration: 0.28, ease: 'easeOut' }}
            style={{ background: palette.fill }}
          >
            {/* 顶部 1px catch-light —— 手绘高光线 */}
            <div
              className="absolute inset-x-0 top-0 pointer-events-none"
              style={{ height: 1, background: palette.lit }}
              aria-hidden
            />
            {/* 治疗扫光 —— HP 上升时浅羊皮色斜带扫一次(暖色,不是塑料白光) */}
            {shimmerToken > 0 && (
              <div
                key={shimmerToken}
                className="absolute inset-0 pointer-events-none"
                style={{
                  background:
                    'linear-gradient(115deg, transparent 35%, rgba(247,233,190,0.4) 50%, transparent 65%)',
                  animation: 'hp-shimmer 0.8s ease-out 1',
                }}
              />
            )}
          </motion.div>

          {/* 25/50/75 仪表刻度 —— 暗色细线 + 1px 浅边,平面雕刻感。
              槽的兄弟层(叠在填充之上),不受填充 scaleX 影响 */}
          {TICKS.map((tick) => (
            <div
              key={tick}
              className="absolute pointer-events-none"
              style={{
                left: `${tick}%`,
                top: 1,
                bottom: 1,
                width: 1,
                background: 'rgba(10,7,3,0.5)',
                boxShadow: '1px 0 0 rgba(231,221,191,0.12)',
              }}
              aria-hidden
            />
          ))}

          {/* Critical —— 框内石榴红细环呼吸(inset ring,无外发光),跟浮空血条同款 */}
          <AnimatePresence>
            {isCritical && (
              <motion.div
                initial={{ opacity: 0 }}
                animate={{ opacity: [0.3, 0.85, 0.3] }}
                exit={{ opacity: 0 }}
                transition={{ duration: 1.4, repeat: Infinity, ease: 'easeInOut' }}
                className="absolute inset-0 pointer-events-none"
                style={{
                  borderRadius: 2,
                  boxShadow: `inset 0 0 0 1px ${garnet(0.95)}, inset 0 0 5px ${garnet(0.45)}`,
                }}
                aria-hidden
              />
            )}
          </AnimatePresence>
        </div>
      </div>

      {/* Shimmer keyframe inline —— 组件私有动画,不进全局 css。one-shot 模式从 -100% → 200%
          走一次就停在屏外(等同消失),下一次 token 变化时 remount 重播。 */}
      <style>{`
        @keyframes hp-shimmer {
          0%   { transform: translateX(-100%); }
          100% { transform: translateX(200%); }
        }
      `}</style>
    </div>
  );
}
