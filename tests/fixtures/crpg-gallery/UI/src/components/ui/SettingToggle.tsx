import { useFocusable } from '@noriginmedia/norigin-spatial-navigation';
import type { SettingDef } from '@/stores/settings-store';

interface Props {
  settingKey: string;
  def: SettingDef;
  value: boolean;
  isActive?: boolean;
  onChange: (key: string, value: boolean) => void;
  onHover: (key: string | null) => void;
}

export function SettingToggle({ settingKey, def, value, isActive, onChange, onHover }: Props) {
  // useFocusable makes the row a navigation target. Enter flips the value;
  // focus also drives the detail panel via onHover so keyboard / gamepad
  // and mouse share the same "what am I looking at" state.
  const { ref, focused } = useFocusable({
    onEnterPress: () => onChange(settingKey, !value),
    onFocus: () => onHover(settingKey),
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
        <button
          type="button"
          className={`settings-toggle ${value ? 'is-on' : ''}`}
          onClick={() => onChange(settingKey, !value)}
          tabIndex={-1}
        >
          <span className="settings-toggle-handle" />
        </button>
      </div>
    </div>
  );
}
