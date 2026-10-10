import { useCallback } from 'react';
import { PlayerHUD } from '@/components/PlayerHUD';
import { useUEHandler } from '@/hooks/use-ue-handler';
import { useGameStore } from '@/stores/game-store';

/**
 * Game HUD page — registers UE handlers for player vitals and info.
 * Handlers are active only while HUD is mounted (page is 'hud').
 *
 * 注意:战斗 SelectedUnit 面板不在这里 — 它是跨页存在的 overlay,跟 SubtitleOverlay
 * 一样在 App.tsx 里始终挂载,handler 也在 App 模块加载时全局注册。这样玩家不在 HUD
 * 页的时候(比如打开了战斗主菜单)选中变化照样推得上。
 */
export function HUD() {
  const { setHealth, setMana, setStamina, setPlayerInfo } = useGameStore.getState();

  useUEHandler('setHealth', useCallback((data: unknown) => {
    const d = data as { current: number; max?: number };
    setHealth(d.current, d.max);
  }, [setHealth]));

  useUEHandler('setMana', useCallback((data: unknown) => {
    const d = data as { current: number; max?: number };
    setMana(d.current, d.max);
  }, [setMana]));

  useUEHandler('setStamina', useCallback((data: unknown) => {
    const d = data as { current: number; max?: number };
    setStamina(d.current, d.max);
  }, [setStamina]));

  useUEHandler('setPlayerInfo', useCallback((data: unknown) => {
    const d = data as { name: string; level: number };
    setPlayerInfo(d.name, d.level);
  }, [setPlayerInfo]));

  return <PlayerHUD />;
}
