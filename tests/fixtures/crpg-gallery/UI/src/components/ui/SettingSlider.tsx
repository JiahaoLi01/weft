import type { CSSProperties } from 'react';
import { useFocusable } from '@noriginmedia/norigin-spatial-navigation';
import type { SettingDef } from '@/stores/settings-store';

interface Props {
  settingKey: string;
  def: SettingDef;
  value: number;
  isActive?: boolean;
  onChange: (key: string, value: number) => void;
  onHover: (key: string | null) => void;
}

export function SettingSlider({ settingKey, def, value, isActive, onChange, onHover }: Props) {
  const min = def.min ?? 0;
  const max = def.max ?? 1;
  const step = def.step ?? 0.01;

  const display =
    max <= 1
      ? `${Math.round(value * 100)}%`
      : typeof value === 'number'
        ? value.toFixed(step < 1 ? 1 : 0)
        : String(value);

  const fillPct = max > min ? ((value - min) / (max - min)) * 100 : 0;
  const sliderStyle = { '--fill': `${fillPct}%` } as CSSProperties;

  // Spatial-nav focus + arrow handling. Left/right adjust value (and we
  // block the lib from also navigating), up/down let the lib resolve as
  // normal so the player can move between rows.
  const { ref, focused } = useFocusable({
    onFocus: () => onHover(settingKey),
    onArrowPress: (direction) => {
      if (direction === 'left' || direction === 'right') {
        const delta = direction === 'right' ? step : -step;
        const next = Math.min(max, Math.max(min, +(value + delta).toFixed(6)));
        onChange(settingKey, next);
        return false;  // consume — don't also move focus sideways
      }
      return true;     // up/down: let spatial nav handle
    },
  });

  return (
    <div
      ref={ref}
      className={`settings-row ${isActive || focused ? 'is-active' : ''} ${focused ? 'is-focused' : ''}`}
      onMouseEnter={() => onHover(settingKey)}
      onMouseLeave={() => onHover(null)}
    >
      <span className="settings-row-label">{def.label}</span>
      <div className="settings-row-control">
        <input
          type="range"
          className="settings-slider"
          style={sliderStyle}
          min={min}
          max={max}
          step={step}
          value={value}
          onChange={e => onChange(settingKey, parseFloat(e.target.value))}
          tabIndex={-1}
        />
        <span className="settings-row-value">{display}</span>
      </div>
    </div>
  );
}
