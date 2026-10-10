import React, { createContext, useContext, useEffect, useRef } from 'react';
import { sendToUE } from '@/lib/ue-bridge';
import { command } from '@/lib/format';
import { Hoverable } from './tooltip/TooltipSystem';
import { NineSlice, panelArt } from './NineSlice';

/**
 * Where a panel is drawn (HierarchyUI.md 第 5 节): docked = the dossier beside the business tree, or a workbench in its
 * place (wide); the map stays live around it. Not docked = a dialog over a dimmed map (settings).
 */
export const DockContext = createContext<{ docked: boolean; wide?: boolean }>({ docked: false });

// Esc closes the panel on top only (a workbench over the dossier).
const escStack: { current: () => void }[] = [];

/** A term with its explanation on hover (dotted underline). */
export function Term({ name, children }: { name: string; children: React.ReactNode }) {
  return <Hoverable content={children}><span className="term">{name}</span></Hoverable>;
}

/**
 * An operating panel. Docked (the dossier and the workbenches, HierarchyUI.md 第 6 节) it stands beside the business tree
 * and the map stays live; otherwise it is a dialog that owns every pointer event (data-ui-modal plus the UE input lock).
 * Esc closes the panel on top; Space still pauses the clock.
 */
export function Modal({ eyebrow, title, meta, figures, actions, tabs, children, onClose, className = '', compact = false, layer = '' }: {
  eyebrow: React.ReactNode; title: React.ReactNode; meta?: React.ReactNode; figures?: React.ReactNode; actions?: React.ReactNode;
  tabs?: React.ReactNode; children: React.ReactNode; onClose: () => void; className?: string;
  /** A class for the layer under a dialog (the tech tree's fills the screen, over the top bar). */
  layer?: string;
  /** Docked: as tall as its content (up to the full height) instead of always full height. */
  compact?: boolean;
}) {
  const { docked, wide } = useContext(DockContext);
  useEffect(() => {
    if (docked) return;
    sendToUE('ui_modal', { open: true });
    return () => sendToUE('ui_modal', { open: false });
  }, [docked]);
  const close = useRef(onClose);
  close.current = onClose;
  useEffect(() => {
    const entry = { current: () => close.current() };
    escStack.push(entry);
    const onKey = (e: KeyboardEvent) => {
      const typing = e.target instanceof HTMLInputElement || e.target instanceof HTMLSelectElement || e.target instanceof HTMLTextAreaElement;
      if (e.key === 'Escape' && escStack[escStack.length - 1] === entry) { e.preventDefault(); entry.current(); }
      else if (e.key === ' ' && !typing && escStack[escStack.length - 1] === entry) { e.preventDefault(); command('time.toggle_pause'); }
    };
    window.addEventListener('keydown', onKey);
    return () => { window.removeEventListener('keydown', onKey); escStack.splice(escStack.indexOf(entry), 1); };
  }, []);
  const content = <div className="modal-content">
    <header className="modal-head">
      <div className="modal-title"><small>{eyebrow}</small><h1>{title}</h1>{meta && <div className="modal-meta">{meta}</div>}</div>
      {figures && <div className="modal-figures">{figures}</div>}
      <div className="modal-actions">{actions}<button className="link modal-close" onClick={onClose} aria-label="关闭">{docked ? '收起 ›' : '关闭 ×'}</button></div>
    </header>
    {tabs && <nav className="modal-tabs">{tabs}</nav>}
    <div className="modal-body">{children}</div>
  </div>;
  if (docked) return <section className={`dossier-layer ${wide ? 'wide' : ''} ${compact ? 'compact' : ''}`} aria-label="档案">
    <NineSlice art={panelArt.card} className={`dossier-panel modal-panel ${className}`} role="region">{content}</NineSlice>
  </section>;
  return <div className={`modal-layer ${layer}`} data-ui-modal="true">
    <section className={`modal-panel painted ${className}`} role="dialog" aria-modal="true">{content}</section>
  </div>;
}

export function Tabs<T extends string>({ tabs, value, onChange }: { tabs: { id: T; name: string; badge?: React.ReactNode }[]; value: T; onChange: (v: T) => void }) {
  return <>{tabs.map(t => <button key={t.id} className={value === t.id ? 'active' : ''} aria-pressed={value === t.id} onClick={() => onChange(t.id)}>{t.name}{t.badge !== undefined && <em>{t.badge}</em>}</button>)}</>;
}

/** Figure cell for modal headers and overview grids. */
export function Cell({ label, value, note, tone }: { label: React.ReactNode; value: React.ReactNode; note?: React.ReactNode; tone?: string }) {
  return <div className={`cell ${tone ?? ''}`}><small>{label}</small><b>{value}</b>{note && <span>{note}</span>}</div>;
}

/** Status words as a quiet badge; tone from the words (受限、封存、安装、爬坡). */
export function StatusBadge({ status, tone }: { status: string; tone: string }) {
  return <span className={`status-badge ${tone}`}>{status}</span>;
}

/** Numeric input that keeps its own text while typing. */
export function MoneyInput({ value, onChange, label, width = 90 }: { value: string; onChange: (v: string) => void; label?: string; width?: number }) {
  return <label className="money-input">{label}<span>£</span><input value={value} style={{ width }} inputMode="decimal" onChange={e => onChange(e.target.value.replace(/[^0-9.]/g, ''))} /></label>;
}
