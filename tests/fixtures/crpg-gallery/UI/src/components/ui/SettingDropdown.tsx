import { useState, useEffect } from 'react';
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

export function SettingDropdown({ settingKey, def, value, isActive, onChange, onHover }: Props) {
  const options = def.options ?? [];
  const [open, setOpen] = useState(false);
  // Highlighted index while the menu is open. Decoupled from `value` so the
  // player can browse options without committing — Enter commits, Escape /
  // outside-click discards.
  const [highlight, setHighlight] = useState(value);

  // Close on Escape while open. Listening at the window level because focus
  // stays on the dropdown trigger; the menu items are visual only and not
  // separate focus targets — see the design note in onArrowPress below.
  useEffect(() => {
    if (!open) return;
    const handler = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.stopPropagation();
        setOpen(false);
      }
    };
    window.addEventListener('keydown', handler, true);
    return () => window.removeEventListener('keydown', handler, true);
  }, [open]);

  // ── Spatial-nav integration ─────────────────────────────────────────
  //
  // Strategy: focus stays on the trigger button while the menu is open.
  // We track `highlight` in local state and consume arrow up/down when
  // open to cycle options. This is simpler than making each menu item a
  // separate focus target (and avoids the "where do I land when I close?"
  // problem — closing always returns to the trigger naturally).
  //
  //   Closed + Enter   → open menu, seed highlight from current value
  //   Open   + Up/Down → cycle highlight, block sideways nav
  //   Open   + L/R     → consume so left/right can't tab to another row
  //   Open   + Enter   → commit highlight, close menu
  //   Open   + Escape  → close menu (handled in the effect above)
  const { ref, focused } = useFocusable({
    onFocus: () => onHover(settingKey),
    onEnterPress: () => {
      if (open) {
        if (highlight !== value) onChange(settingKey, highlight);
        setOpen(false);
      } else {
        setHighlight(value);
        setOpen(true);
      }
    },
    onArrowPress: (direction) => {
      if (!open) return true;
      if (direction === 'down') {
        setHighlight(i => Math.min(options.length - 1, i + 1));
        return false;
      }
      if (direction === 'up') {
        setHighlight(i => Math.max(0, i - 1));
        return false;
      }
      // left/right: nothing to do, but consume so the spatial nav doesn't
      // jump to another row while the menu is "modal" over this row.
      return false;
    },
  });

  // Outside-click handler reuses the same RefObject useFocusable returns —
  // norigin gives us a real React ref, so .current is the wrapper node.
  useEffect(() => {
    if (!open) return;
    const handler = (e: MouseEvent) => {
      if (ref.current && !ref.current.contains(e.target as Node)) {
        setOpen(false);
      }
    };
    document.addEventListener('mousedown', handler);
    return () => document.removeEventListener('mousedown', handler);
  }, [open, ref]);

  const current = options[value] ?? '—';

  return (
    <div
      ref={ref}
      className={`settings-row ${isActive || focused ? 'is-active' : ''} ${focused ? 'is-focused' : ''}`}
      onMouseEnter={() => onHover(settingKey)}
      onMouseLeave={() => onHover(null)}
    >
      <span className="settings-row-label">{def.label}</span>
      <div className="settings-row-control">
        <div className="settings-dropdown">
          <button
            type="button"
            className="settings-dropdown-trigger"
            onClick={() => {
              if (open) setOpen(false);
              else { setHighlight(value); setOpen(true); }
            }}
            tabIndex={-1}
          >
            <span>{current}</span>
            <span className="settings-dropdown-arrow">{open ? '▲' : '▼'}</span>
          </button>
          {open && (
            <div className="settings-dropdown-menu">
              {options.map((opt, i) => (
                <button
                  key={i}
                  type="button"
                  className={
                    `settings-dropdown-option` +
                    (i === value ? ' is-selected' : '') +
                    (i === highlight ? ' is-highlight' : '')
                  }
                  onClick={() => {
                    onChange(settingKey, i);
                    setOpen(false);
                  }}
                  onMouseEnter={() => setHighlight(i)}
                  tabIndex={-1}
                >
                  {opt}
                </button>
              ))}
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
