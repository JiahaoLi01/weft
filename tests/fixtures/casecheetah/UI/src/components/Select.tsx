import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { getLayoutRect, getZoom } from '@/components/tooltip/TooltipSystem';
import { useLogicalViewport } from '@/lib/use-logical-viewport';
import { usePageActive } from '@/components/PageActivity';

export interface SelectOption { value: string; label: string; disabled?: boolean }

/**
 * 自绘下拉（移植 CaseCat 的 Select）。Ultralight 把原生 <select> 画成系统灰的控件和白底弹层，不吃页面样式。
 * 展开时铺一层透明背板，点外面收起；Esc 也收起；列表限高滚动，下方放不下时向上展开。
 * 不要把它放进 <label>：标签会把点击转给触发按钮，收起后立刻重开。
 */
export function Select({ value, onChange, options, ariaLabel, minWidth = 150 }: {
  value: string; onChange: (value: string) => void; options: SelectOption[]; ariaLabel?: string; minWidth?: number;
}) {
  const active = usePageActive();
  const [open, setOpen] = useState(false);
  const [upward, setUpward] = useState(false);
  const viewport = useLogicalViewport();
  const anchor = useRef<HTMLButtonElement>(null);
  const [position, setPosition] = useState({ left: 0, top: 0, width: minWidth, maxHeight: 220 });
  const selected = options.find(o => o.value === value);

  useLayoutEffect(() => { if (!active) setOpen(false); }, [active]);
  useLayoutEffect(() => {
    if (!active || !open || !anchor.current) return;
    const place = () => {
      if (!anchor.current) return;
      const rect = getLayoutRect(anchor.current.getBoundingClientRect());
      const vh = window.innerHeight / getZoom(), vw = window.innerWidth / getZoom();
      const above = Math.max(0, rect.top - 12), below = Math.max(0, vh - rect.bottom - 12);
      const up = below < 160 && above > below;
      const width = Math.min(Math.max(rect.width, minWidth), Math.max(0, vw - 16));
      setPosition({ left: Math.max(8, Math.min(rect.left, vw - width - 8)), top: up ? rect.top - 4 : rect.bottom + 4, width, maxHeight: Math.min(240, up ? above : below) });
      setUpward(up);
    };
    place();
    const dismiss = (event: Event) => { if (!(event.target instanceof Element) || !event.target.closest('[data-select-menu]')) setOpen(false); };
    window.addEventListener('resize', place);
    window.addEventListener('scroll', dismiss, true);
    return () => { window.removeEventListener('resize', place); window.removeEventListener('scroll', dismiss, true); };
  }, [active, open, minWidth, viewport.width, viewport.height]);
  useEffect(() => {
    if (!active || !open) return;
    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); setOpen(false); anchor.current?.focus(); }
    };
    // Capture phase: Esc closes the list before the panel underneath sees it.
    window.addEventListener('keydown', onKey, true);
    return () => window.removeEventListener('keydown', onKey, true);
  }, [active, open]);

  return <span className="select">
    <button ref={anchor} type="button" className="select-trigger" aria-haspopup="listbox" aria-expanded={open} aria-label={ariaLabel} style={{ minWidth }} onClick={() => setOpen(!open)}>
      <span>{selected ? selected.label : '—'}</span><i aria-hidden="true">{upward && open ? '▴' : '▾'}</i>
    </button>
    {active && open && createPortal(<>
      <div className="select-backdrop" onClick={(event) => { event.preventDefault(); setOpen(false); anchor.current?.focus(); }} />
      <div data-select-menu role="listbox" className="select-menu" style={{ ...position, transform: upward ? 'translateY(-100%)' : undefined }}>
        {options.map(o => <button key={o.value} type="button" role="option" aria-selected={o.value === value} disabled={o.disabled}
          className={o.value === value ? 'selected' : ''} onClick={() => { onChange(o.value); setOpen(false); }}>{o.label}</button>)}
        {!options.length && <span className="select-empty">无可选项</span>}
      </div>
    </>, document.body)}
  </span>;
}
