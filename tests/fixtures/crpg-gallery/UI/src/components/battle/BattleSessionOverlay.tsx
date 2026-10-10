/**
 * BattleSessionOverlay —— 战斗会话常驻控件的统一挂载点。
 *
 * C++ UBattleUIDispatcher OnActivated / OnDeactivated 推 `setBattleSession`,
 * 这里按 `session.active` 决定子控件是否存在 —— 战斗外这些组件**完全不在 DOM 里**,
 * 而不是各自挂在 App 根下靠内部 return null 自我隐藏(之前 EndTurnButton 的写法)。
 * 新增战斗常驻控件(回合指示器、行动顺序条等)挂这里,别再散装挂回 App 根。
 *
 * 子控件都是 z-auto 兄弟(各自 absolute 定位),被 page stack(zIndex ≥ 10,
 * Settings 等)正常覆盖 —— 不要给任何子控件加显式 z-index 跳出栈管理。
 */

import { useBattleStore } from '@/stores/battle-store';
import { EndTurnButton } from '@/components/battle/EndTurnButton';
import { TurnBanner } from '@/components/battle/TurnBanner';

export function BattleSessionOverlay() {
  const active = useBattleStore((s) => s.session.active);
  if (!active) {
    return null;
  }
  return (
    <>
      <EndTurnButton />
      <TurnBanner />
    </>
  );
}
