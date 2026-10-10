import { useGameStore } from '@/stores/game-store';
import { useT } from '@/stores/i18n-store';
import { VitalBar } from './VitalBar';

export function PlayerHUD() {
  const {
    playerName, level,
    health, maxHealth,
    mana, maxMana,
    stamina, maxStamina,
    connected,
  } = useGameStore();
  const t = useT();

  return (
    <div className="absolute left-4 top-4 w-64 bg-panel rounded border border-panel-border p-3 flex flex-col gap-2 backdrop-blur-sm">
      {/* Header */}
      <div className="flex items-center justify-between">
        <span className="font-[family-name:var(--font-display)] text-gold text-sm font-semibold">
          {playerName}
        </span>
        <span className="text-xs text-parchment-dark">Lv.{level}</span>
      </div>

      {/* Vital Bars */}
      <VitalBar label="HP" current={health} max={maxHealth} color="bg-health" />
      <VitalBar label="MP" current={mana} max={maxMana} color="bg-mana" />
      <VitalBar label="SP" current={stamina} max={maxStamina} color="bg-stamina" />

      {/* Connection indicator (dev only) */}
      {!connected && (
        <div className="text-[10px] text-blood-light text-center mt-1">
          {t('hud.not_connected')}
        </div>
      )}
    </div>
  );
}
