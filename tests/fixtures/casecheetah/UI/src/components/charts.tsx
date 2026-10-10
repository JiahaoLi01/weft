import React, { useEffect, useRef, useState } from 'react';
import { Hoverable, getLayoutRect, getZoom } from './tooltip/TooltipSystem';
import { isInUnreal } from '../lib/ue-bridge';

// Charts drawn as plain SVG / DOM so Ultralight renders them without a canvas. The demo's vocabulary: line charts with
// a dashed grid, sparklines, 14-segment utilisation bars, skill tracks with a requirement marker, proportional bars.

const ULTRALIGHT = isInUnreal();

/** Pointer x inside an element in its own layout pixels (Ultralight: rects are logical, clientX is physical). */
function layoutX(event: React.MouseEvent, element: Element): { x: number; width: number } {
  const z = getZoom();
  const r = element.getBoundingClientRect();
  return ULTRALIGHT ? { x: event.clientX / z - r.left, width: r.width } : { x: (event.clientX - r.left) / z, width: r.width / z };
}

/** Layout width of an element, re-measured on resize and shortly after mount (tabs and panels settle). */
function useWidth(ref: React.RefObject<HTMLElement | null>, fallback: number) {
  const [width, setWidth] = useState(fallback);
  useEffect(() => {
    const measure = () => { if (ref.current) { const w = getLayoutRect(ref.current.getBoundingClientRect()).width; if (w > 40) setWidth(Math.round(w)); } };
    measure();
    const timer = setTimeout(measure, 120);
    window.addEventListener('resize', measure);
    return () => { clearTimeout(timer); window.removeEventListener('resize', measure); };
  }, [ref]);
  return width;
}

/**
 * Dashed polyline as explicit segments. Ultralight keeps the last SVG dash pattern for every later stroke on the page
 * (even stroke-dasharray="none" is ignored), so no chart uses stroke-dasharray at all.
 */
export function dashPath(points: [number, number][], dash: number, gap: number): string {
  let d = '', on = true, left = dash;
  for (let i = 1; i < points.length; i++) {
    let [x0, y0] = points[i - 1];
    const [x1, y1] = points[i];
    let length = Math.hypot(x1 - x0, y1 - y0);
    while (length > 1e-6) {
      const step = Math.min(left, length), t = step / length;
      const xs = x0 + (x1 - x0) * t, ys = y0 + (y1 - y0) * t;
      if (on) d += `M${x0.toFixed(1)},${y0.toFixed(1)}L${xs.toFixed(1)},${ys.toFixed(1)}`;
      x0 = xs; y0 = ys; length -= step; left -= step;
      if (left <= 1e-6) { on = !on; left = on ? dash : gap; }
    }
  }
  return d;
}

export type Series = { key: string; name: string; color: string; wide?: boolean; dashed?: boolean };
export type ChartRow = { date: string; shade?: boolean } & Record<string, number | string | boolean | undefined>;

/** Weekly line chart: grid at the extremes and zero, three date ticks, optional shaded weeks, hover readout. */
export function LineChart({ rows, series, format, label, empty = '推进几周后显示走势。', height = 200, unitHint }: {
  rows: ChartRow[]; series: Series[]; format: (v: number) => string; label: string; empty?: string; height?: number; unitHint?: string;
}) {
  const [hover, setHover] = useState<number | null>(null);
  const ref = useRef<SVGSVGElement>(null);
  const wrap = useRef<HTMLDivElement>(null);
  const measured = useWidth(wrap, 600);
  if (rows.length < 2) return <div className="chart-empty" ref={wrap}>{empty}</div>;
  const W = Math.max(280, measured), H = height, L = 52, R = 12, T = 12, B = 24;
  const values = rows.flatMap(r => series.map(s => r[s.key])).filter((v): v is number => typeof v === 'number' && Number.isFinite(v));
  let max = Math.max(0, ...values), min = Math.min(0, ...values);
  if (max === min) max = min + 1;
  const pad = (max - min) * .08;
  max += pad; if (min < 0) min -= pad;
  const x = (i: number) => L + (W - L - R) * i / (rows.length - 1);
  const y = (v: number) => H - B - (H - T - B) * (v - min) / (max - min);
  const ticks = Array.from(new Set([min, min < 0 && max > 0 ? 0 : (min + max) / 2, max]));
  const dates = [0, Math.floor((rows.length - 1) / 2), rows.length - 1];
  const step = (W - L - R) / (rows.length - 1);
  const onMove = (e: React.MouseEvent) => {
    if (!ref.current) return;
    const { x: px, width } = layoutX(e, ref.current);
    const vx = px * W / Math.max(1, width);
    setHover(Math.max(0, Math.min(rows.length - 1, Math.round((vx - L) / step))));
  };
  const hx = hover !== null ? x(hover) : 0;
  return <div className="chart-wrap" ref={wrap}>
    <svg ref={ref} className="chart" viewBox={`0 0 ${W} ${H}`} width={W} height={H} role="img" aria-label={label} onMouseMove={onMove} onMouseLeave={() => setHover(null)}>
      {rows.map((r, i) => r.shade ? <rect key={'s' + i} x={x(i) - step / 2} y={T} width={step} height={H - T - B} className="shade" /> : null)}
      {ticks.map((t, i) => <g key={'t' + i}>{t === 0 ? <line className="zero" x1={L} x2={W - R} y1={y(t)} y2={y(t)} /> : <path className="grid" d={dashPath([[L, y(t)], [W - R, y(t)]], 3, 6)} />}<text x={L - 8} y={y(t) + 3} textAnchor="end">{format(t)}</text></g>)}
      {series.map(s => {
        const points = rows.flatMap((r, i): [number, number][] => typeof r[s.key] === 'number' ? [[x(i), y(r[s.key] as number)]] : []);
        return s.dashed
          ? <path key={s.key} className="series" stroke={s.color} strokeWidth={s.wide ? 2.6 : 1.7} d={dashPath(points, 6, 5)} />
          : <polyline key={s.key} className="series" stroke={s.color} strokeWidth={s.wide ? 2.6 : 1.7} points={points.map(p => `${p[0].toFixed(1)},${p[1].toFixed(1)}`).join(' ')} />;
      })}
      {dates.map((i, j) => <text key={'d' + j} x={x(i)} y={H - 7} textAnchor={j === 0 ? 'start' : j === 2 ? 'end' : 'middle'}>{rows[i].date}</text>)}
      {hover !== null && <g className="cursor">
        <line x1={hx} x2={hx} y1={T} y2={H - B} />
        {series.map(s => typeof rows[hover][s.key] === 'number' ? <circle key={s.key} cx={hx} cy={y(rows[hover][s.key] as number)} r={3.2} fill={s.color} /> : null)}
      </g>}
      <rect x={L} y={T} width={W - L - R} height={H - T - B} fill="transparent" />
    </svg>
    <div className="chart-readout">
      {hover !== null ? <><b>{rows[hover].date}</b>{series.map(s => typeof rows[hover][s.key] === 'number' ? <span key={s.key}><i style={{ background: s.color }} />{s.name} {format(rows[hover][s.key] as number)}</span> : null)}</>
        : <>{series.map(s => <span key={s.key}><i className={s.dashed ? 'dashed' : ''} style={{ background: s.color }} />{s.name}</span>)}{unitHint && <em>{unitHint}</em>}</>}
    </div>
  </div>;
}

/** Small trend line; a baseline at zero when values cross it. */
export function Sparkline({ values, color = '#c9b27c', width = 92, height = 24, label }: { values: number[]; color?: string; width?: number; height?: number; label?: string }) {
  const W = width, H = height;
  const max = Math.max(0, ...values), min = Math.min(0, ...values);
  const span = max - min || 1;
  const x = (i: number) => 2 + (W - 4) * i / Math.max(1, values.length - 1);
  const y = (v: number) => H - 3 - (H - 6) * (v - min) / span;
  return <svg className="spark" viewBox={`0 0 ${W} ${H}`} width={W} height={H} role="img" aria-label={label}>
    <line x1={2} x2={W - 2} y1={y(0)} y2={y(0)} className="base" />
    {values.length > 1 && <polyline fill="none" stroke={color} strokeWidth={1.7} strokeLinejoin="round" strokeLinecap="round" points={values.map((v, i) => `${x(i).toFixed(1)},${y(v).toFixed(1)}`).join(' ')} />}
  </svg>;
}

/** Fourteen segments lit in proportion to output against capacity. */
export function UtilBars({ ratio, segments = 14 }: { ratio: number; segments?: number }) {
  const lit = Math.round(Math.max(0, Math.min(1, ratio)) * segments);
  return <div className="util-bars" aria-label={`产能利用 ${Math.round(ratio * 100)}%`}>{Array.from({ length: segments }, (_, i) => <i key={i} className={i < lit ? 'on' : ''} />)}</div>;
}

/** 手艺（Production.md 第 1 节）：a crew's proficiency (0..100) in each craft — the ones its line runs with their stage or component,
 *  then the ones it is forgetting. */
export function CraftBars({ crafts }: { crafts: { id: string; name: string; part: string; value: number; inUse: boolean }[] }) {
  return <div className="skill-list">{crafts.map(c => <div key={c.id} className={c.inUse ? '' : 'fading'}>
    <div className="skill-head"><span>{c.inUse ? `${c.part} · ${c.name}` : `${c.name}（不用了，渐忘）`}</span><b>{Math.round(c.value)}</b></div>
    <div className="skill-track"><span style={{ width: `${Math.min(100, c.value)}%` }} /></div>
  </div>)}</div>;
}

export type BarPart = { key: string; label: string; value: number; color: string; detail?: string };
/** Proportional bar: wide parts carry their label, every part explains itself on hover. */
export function HBar({ parts, empty = '—' }: { parts: BarPart[]; empty?: string }) {
  const shown = parts.filter(p => p.value > 0);
  const total = shown.reduce((a, p) => a + p.value, 0);
  if (!total) return <div className="hbar empty"><span>{empty}</span></div>;
  return <div className="hbar">{shown.map(p => {
    const s = p.value / total;
    return <div key={p.key} className="hbar-cell" style={{ flexGrow: s }}>
      <Hoverable className="hbar-hit" content={<>{p.label} · {Math.round(s * 100)}%{p.detail ? <><br />{p.detail}</> : null}</>} pinnable={false}>
        <span className="hbar-part" style={{ background: p.color }}>{s >= .16 ? `${p.label} ${Math.round(s * 100)}%` : s >= .08 ? `${Math.round(s * 100)}%` : ''}</span>
      </Hoverable>
    </div>;
  })}</div>;
}

export function Legend({ parts }: { parts: { label: string; color: string; detail?: string }[] }) {
  return <div className="viz-legend">{parts.map(p => <span key={p.label}><i style={{ background: p.color }} />{p.label}{p.detail ? <em>{p.detail}</em> : null}</span>)}</div>;
}

/** Label / value pairs, values right-aligned in the figure font. */
export function StatPairs({ rows }: { rows: [React.ReactNode, React.ReactNode, string?][] }) {
  return <div className="stat-pairs">{rows.map(([k, v, tone], i) => <React.Fragment key={i}><span>{k}</span><b className={tone}>{v}</b></React.Fragment>)}</div>;
}

/** A quiet progress track with a label and figure above it. */
export function Meter({ label, value, text, tone }: { label: React.ReactNode; value: number; text: React.ReactNode; tone?: string }) {
  return <div className={`meter ${tone ?? ''}`}><div className="meter-head"><span>{label}</span><b>{text}</b></div><div className="meter-track"><span style={{ width: `${Math.max(0, Math.min(1, value)) * 100}%` }} /></div></div>;
}

/** Big figure with a caption (the line card's readout). */
export function Figure({ value, caption, tone }: { value: React.ReactNode; caption: React.ReactNode; tone?: string }) {
  return <div className={`figure ${tone ?? ''}`}><strong>{value}</strong><small>{caption}</small></div>;
}

export const palette = { gold: '#d9bd7c', ivory: '#e8e2cf', teal: '#7fb3a8', rust: '#c98b74', violet: '#9d8fc0', slate: '#8796a0', green: '#8fb47c', sand: '#b9a37a' };
