import { useState } from 'react';
import { useBattleStore } from '@/stores/battle-store';
import { sendToUE } from '@/lib/ue-bridge';
import { useEndTurnLabel } from '@/i18n/battle-labels';
import { brass } from '@/config/battle-palette';

/**
 * EndTurnButton —— 右上角"结束轮次"按钮。
 *
 * **挂载点:BattleSessionOverlay**(battle session active 时才存在)—— 不再直挂 App 根。
 * 内部的 factionTag 空检查保留作防御(session 已开但 TurnOrder 空的边缘态)。
 *
 * 数据流:
 *   - **In**: `battleStore.currentTurn` 由 C++ `setBattleTurn` 推送维护。factionTag 为空字符串
 *     时按钮不渲染(TurnOrder 空等边缘态)。
 *   - **Out**: 点击发 `sendToUE('battleUI.endTurnClicked', {})` → `UBattleUIDispatcher::
 *     HandleEndTurnClicked` → `BIWS::RequestEndTurn` → `RoundManager::AdvanceTurn`。
 *
 * **没有内置 confirm 对话框** —— 误点的代价是"轮次推进了一格",影响有限(BG3 同款行为,
 * 误点了下次回合再来)。需要 confirm 的场景(玩家还没行动 / 还有可用动作经济等),后续在
 * C++ 端 RequestEndTurn 内部加 guard,UI 端依旧只发 intent。
 *
 * # 样式细节
 *
 * 项目笔记 `[ultralight_css_hover_unreliable.md]`:absolute-positioned 组件在 transparent
 * body 上 `:hover` 闪烁不稳,**手动 useState + onMouseEnter/Leave/Down/Up 管理**。
 * 这里走 hover/press 双状态,跟 BattleUnitPanel 的 SkillCard 同 pattern。
 *
 * 不挂入 spatial-navigation —— End Turn 是"重要但低频"的全局按钮,用 mouse / 专用键盘
 * 快捷键更合理(后续可以加 hotkey),不该挤进角色面板的方向键焦点链里。
 */

export function EndTurnButton()
{
  const currentTurn = useBattleStore((s) => s.currentTurn);
  const label = useEndTurnLabel();

  const [isHovered, setIsHovered] = useState(false);
  const [isPressed, setIsPressed] = useState(false);

  // **factionTag 空 = BattleUIDispatcher 没激活 / 战斗外 → 整个按钮不渲染**,而不是 disabled
  // 还显示。原因:End Turn 是战斗专属 UI,战斗外它根本不该出现在屏幕上。Dispatcher 的
  // OnDeactivated_Implementation 会主动推一次 factionTag="" 让这条件成立。
  if (!currentTurn.factionTag)
  {
    return null;
  }

  const handleClick = () =>
  {
    sendToUE('battleUI.endTurnClicked', {});
  };

  // 视觉:三态雕版按钮(battle-palette 语言)—— 平面墨色递进(idle → 暖琥珀 hover →
  // 按压加深)+ 黄铜细线 + 内圈回声细线(双线规,呼应战斗面板外框 / SkillCard active 态)。
  // 之前的 2px 粗边、内嵌斜面、光泽内高光已移除。
  let bgColor: string;
  let borderColor: string;
  let textColor: string;

  if (isPressed)
  {
    bgColor = 'rgba(50, 38, 21, 0.95)';
    borderColor = brass(0.7);
    textColor = '#f5e6c8';
  }
  else if (isHovered)
  {
    bgColor = 'rgba(64, 50, 28, 0.92)';
    borderColor = brass(0.85);
    textColor = '#f5e6c8';
  }
  else
  {
    bgColor = 'rgba(32, 25, 16, 0.88)';
    borderColor = brass(0.45);
    textColor = '#d8c8a8';
  }

  return (
    // **absolute + 无显式 z-index** —— 跟 BattleUnitPanel / StatusPanel 等其它常驻战斗
    // HUD 一样,作为 App 根容器的 z-auto 兄弟按 DOM 顺序叠放,被 page stack(zIndex ≥ 10,
    // Settings / 任何 push 的页面)正常覆盖。之前写的 `fixed z-40` 让它游离出栈管理、
    // 永远压在 Settings 之上 —— 不要再加回显式 z-index。
    <div
      className="absolute top-6 right-6 select-none"
      style={{ fontFamily: 'var(--font-display)' }}
    >
      <button
        type="button"
        onMouseEnter={() => setIsHovered(true)}
        onMouseLeave={() => { setIsHovered(false); setIsPressed(false); }}
        onMouseDown={() => setIsPressed(true)}
        onMouseUp={() => setIsPressed(false)}
        onClick={handleClick}
        style={{
          padding: '0.5em 1.3em',
          fontSize: '1em',
          fontWeight: 600,
          letterSpacing: '0.1em',
          background: bgColor,
          color: textColor,
          border: `1px solid ${borderColor}`,
          borderRadius: 3,
          cursor: 'pointer',
          // 内圈回声细线(0 blur)+ 贴地小投影(世界之上的可读性,非装饰深度)
          boxShadow: `inset 0 0 0 1px ${brass(isHovered || isPressed ? 0.25 : 0.15)}, 0 2px 6px rgba(0,0,0,0.5)`,
          // 颜色 / 边框过渡平滑(framer-motion 在这种单点 micro-interaction 上 overkill,纯 CSS 够)
          transition: 'background 120ms ease, border-color 120ms ease, color 120ms ease, box-shadow 120ms ease',
          textShadow: '0 1px 1px rgba(0,0,0,0.5)',
        }}
      >
        {label}
      </button>
    </div>
  );
}
