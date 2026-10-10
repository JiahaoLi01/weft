import { useEffect, useState } from 'react';
import { AnimatePresence, motion } from 'framer-motion';
import { useBattleStore } from '@/stores/battle-store';
import { useSettingsStore } from '@/stores/settings-store';
import { brass, TONE_INK } from '@/config/battle-palette';

/**
 * MovePreviewTooltip —— 移动消耗预览 tooltip,跟随鼠标。
 *
 * # 数据流
 *
 *   C++ UBattleMoveController.UpdateHover → diff broadcast `Ability.Intent.MovePreviewUpdated`
 *     → BattleUIDispatcher → CallUI("setBattleMovePreview", ...)
 *     → store.movePreview {active, reachable, costCm, availableCm}
 *     → 本组件订阅渲染
 *
 * # 显示策略
 *
 *   active = false                    → 完全 return null
 *   active = true && reachable = true → 显示"5.00 m"绿色 tooltip(可走)
 *   active = true && reachable = false→ 显示"5.00 / 3.00 m"红色 tooltip,带"超出范围"
 *
 * # 单位转换
 *
 *   C++ 端是 cm(`Resource.BasicResource.Movement` 资源单位),UI 端 / 100 转米,toFixed(2) 两位
 *   小数。整数 cm 转米永远不会损失精度(500 → 5.00),不需要 round。
 *
 * # 边缘 clamp 思路
 *
 * 跟 `TargetingHoverTooltip` 同一套:默认鼠标右下偏移,溢出 → 反向 flip。两个 tooltip 同时显示时
 * (玩家在移动模式下又 hover 到某个 ability 目标)堆叠位置会有冲突 —— 不在 MVP 范围,以后看
 * 实际使用情况再加 stacking offset。
 */

const OFFSET_X = 18;
const OFFSET_Y = 18;
const MARGIN = 12;

function getZoom(): number
{
  const z = parseFloat(document.documentElement.style.zoom ?? '1');
  return Number.isFinite(z) && z > 0 ? z : 1;
}

function cmToMeters(cm: number): string
{
  return (cm / 100).toFixed(2);
}

export function MovePreviewTooltip()
{
  const preview = useBattleStore((s) => s.movePreview);
  const uiScale = useSettingsStore((s) => s.uiScale);

  const [mousePos, setMousePos] = useState({ x: 0, y: 0 });

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

  // 测 tooltip 自身尺寸做 edge clamp。useEffect 在内容变化时重测。
  const [tooltipSize, setTooltipSize] = useState({ w: 0, h: 0 });
  const setTooltipRef = (el: HTMLDivElement | null) =>
  {
    if (!el) return;
    const rect = el.getBoundingClientRect();
    const z = getZoom();
    const w = rect.width / z;
    const h = rect.height / z;
    if (w !== tooltipSize.w || h !== tooltipSize.h)
    {
      setTooltipSize({ w, h });
    }
  };

  if (!preview.active)
  {
    return null;
  }

  // Edge clamp:默认右下,溢出 → 左 / 上
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
  anchorX = Math.max(MARGIN, Math.min(anchorX, vw - tooltipSize.w - MARGIN));
  anchorY = Math.max(MARGIN, Math.min(anchorY, vh - tooltipSize.h - MARGIN));

  const costM = cmToMeters(preview.costCm);
  const availableM = cmToMeters(preview.availableCm);

  // uiScale 是 effects-only — 引用确保 zoom 变化时 layout 重测
  void uiScale;

  return (
    <AnimatePresence>
      {preview.active && (
        <motion.div
          ref={setTooltipRef}
          key="move-preview"
          initial={{ opacity: 0, scale: 0.92 }}
          animate={{ opacity: 1, scale: 1 }}
          exit={{ opacity: 0, scale: 0.92, transition: { duration: 0.12 } }}
          transition={{ duration: 0.15 }}
          className="fixed pointer-events-none select-none z-50"
          // 无框"铭刻"风格(跟浮空血条铭牌同语言):没有实体面板盒,只有一层羽化的
          // 椭圆暗色衬底保证世界之上的可读性,语义由文字 ink 的色相携带(月桂绿=可达 /
          // 石榴红=超程)。subtitle 混合栈让数字走 Cinzel Decorative 雕花数字,跟目标
          // 计数 / HP 读数同一副。
          style={{
            left: `${anchorX}px`,
            top: `${anchorY}px`,
            fontSize: '0.95em',
            fontFamily: 'var(--font-subtitle)',
            color: preview.reachable ? TONE_INK.positive : TONE_INK.negative,
            // 同浮空血条铭牌的双层 text-shadow —— 紧致暗色剪影,不是光晕
            textShadow: '0 1px 2px rgba(0,0,0,0.9), 0 0 2px rgba(0,0,0,0.7)',
            display: 'flex',
            flexDirection: 'column',
            alignItems: 'center',
            lineHeight: 1.15,
            whiteSpace: 'nowrap',
            // tabular-nums 让数字宽度稳定,鼠标快速 hover 不同距离时数字不跳
            fontVariantNumeric: 'tabular-nums',
          }}
        >
          {/* 羽化暗色衬底 —— 配方同浮空血条铭牌(HealthBar.tsx 注释:Ultralight 忽略
              ellipse 的 size 关键字,用默认 farthest-corner + 在 72% 处归零来模拟
              内切椭圆,宽扁的负 inset 让椭圆贴合文字行)。 */}
          <span
            aria-hidden
            className="absolute pointer-events-none"
            style={{
              inset: '-8px -30px',
              background:
                'radial-gradient(ellipse at center, rgba(0,0,0,0.66) 0%, rgba(0,0,0,0.5) 34%, rgba(0,0,0,0.22) 56%, rgba(0,0,0,0) 72%)',
            }}
          />
          {/* 主距离 —— 两侧黄铜渐隐细线(── 5.37 m ──,雕版铭牌母题) */}
          <div className="relative flex items-center" style={{ fontWeight: 700, fontSize: '1.15em' }}>
            <span
              aria-hidden
              style={{ width: 12, height: 1, marginRight: 6, background: `linear-gradient(90deg, ${brass(0)}, ${brass(0.6)})` }}
            />
            <span>
              {costM}
              <span style={{ fontSize: '0.7em', fontWeight: 600, marginLeft: '0.25em', color: '#d4c4a0' }}>m</span>
            </span>
            <span
              aria-hidden
              style={{ width: 12, height: 1, marginLeft: 6, background: `linear-gradient(270deg, ${brass(0)}, ${brass(0.6)})` }}
            />
          </div>
          {/* 不可达时显示"超出可用范围 X.XX m"细信息 —— 玩家一眼看到差多少 */}
          {!preview.reachable && (
            <div className="relative" style={{ fontSize: '0.68em', marginTop: '0.2em', color: '#d4c4a0' }}>
              超出 / {availableM} m
            </div>
          )}
        </motion.div>
      )}
    </AnimatePresence>
  );
}
