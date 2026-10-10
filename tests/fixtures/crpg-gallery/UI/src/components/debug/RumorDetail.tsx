import { useMemo } from 'react';
import * as d3 from 'd3';
import type { RumorRow, IntensityPoint } from '@/stores/rumor-test-store';
import { lambdaAt, peakTick, variantProbability } from './lambda';

interface RumorDetailProps {
  rumor: RumorRow;
  history: IntensityPoint[];
  currentTick: number;
}

/**
 * 选中 rumor 的细节面板：λ(t) 理论曲线 + I(t) 实测曲线（叠在同一坐标系）+
 * 当前指标 + 权重柱状图 + 耦合列表。所有数学跟模拟器一致。
 */
export function RumorDetail({ rumor, history, currentTick }: RumorDetailProps) {
  // ── 当前指标 ──
  const lambdaNow = useMemo(() => lambdaAt(currentTick, rumor), [currentTick, rumor]);
  const variantP = useMemo(() => variantProbability(rumor.intensity, rumor.variantRate), [rumor]);
  const peak = useMemo(() => peakTick(rumor), [rumor]);

  // ── 时间轴：[birthTick, max(currentTick, birthTick + 5*scale)] ──
  const tMin = Math.max(0, rumor.isBaseline ? 0 : rumor.birthTick);
  const tMax = Math.max(currentTick + 2, tMin + Math.max(10, rumor.scale * 5));

  // 200 个采样点画 λ(t)
  const lambdaPoints = useMemo(() => {
    const n = 200;
    const pts: { tick: number; value: number }[] = [];
    for (let i = 0; i <= n; i += 1) {
      const t = tMin + (tMax - tMin) * (i / n);
      pts.push({ tick: t, value: lambdaAt(t, rumor) });
    }
    return pts;
  }, [tMin, tMax, rumor]);

  return (
    <div className="flex flex-col gap-3">
      {/* Variant 承袭说明——清楚告诉设计师变体内容怎么配。 */}
      {rumor.isVariant && (
        <div className="bg-fuchsia-950/30 border border-fuchsia-800 rounded px-2 py-1.5">
          <div className="text-[10px] text-fuchsia-300 mb-0.5 font-semibold uppercase tracking-wider">
            Variant — runtime-spawned
          </div>
          <div className="text-[10px] text-fuchsia-200/80 leading-relaxed">
            Spawned at tick <span className="font-mono">{rumor.birthTick}</span> from parent{' '}
            <span className="font-mono text-fuchsia-300">{rumor.parentId}</span>.{' '}
            Speaks from <span className="font-mono">VariantExpressions</span> on the parent asset
            (falls back to <span className="font-mono">Expressions</span> if empty).
            Weights are shared with the parent. Only its own curve params (A / k / θ / γ) are independent.
          </div>
        </div>
      )}

      {/* 指标行 */}
      <div className="grid grid-cols-4 gap-2">
        <Metric label="I(t)" value={rumor.intensity.toFixed(2)} />
        <Metric label="λ(t)" value={lambdaNow.toFixed(3)} />
        <Metric label="p_variant" value={(variantP * 100).toFixed(1) + '%'} />
        <Metric label="peak tick" value={peak === null ? 'baseline' : peak.toFixed(1)} />
      </div>

      {/* λ + I 双曲线图 */}
      <CurveChart
        lambdaPoints={lambdaPoints}
        intensityPoints={history}
        tMin={tMin}
        tMax={tMax}
        currentTick={currentTick}
      />

      {/* Tag 权重 */}
      <div>
        <div className="text-[10px] uppercase tracking-wider text-zinc-500 mb-1.5">
          Tag Weights (exp β bar = relative multiplier)
        </div>
        <WeightsBar weights={rumor.weights} />
      </div>

      {/* Incoming Coupling */}
      <div>
        <div className="text-[10px] uppercase tracking-wider text-zinc-500 mb-1.5">
          Incoming Coupling (M[this][other])
        </div>
        <CouplingList coupling={rumor.coupling} />
      </div>
    </div>
  );
}

// ─────────────────── 子组件 ───────────────────

function Metric({ label, value }: { label: string; value: string }) {
  return (
    <div className="bg-zinc-950 border border-zinc-800 rounded px-2 py-1.5">
      <div className="text-[9px] uppercase tracking-wider text-zinc-500">{label}</div>
      <div className="text-sm font-mono text-zinc-200">{value}</div>
    </div>
  );
}

function CurveChart({
  lambdaPoints,
  intensityPoints,
  tMin,
  tMax,
  currentTick,
}: {
  lambdaPoints: { tick: number; value: number }[];
  intensityPoints: IntensityPoint[];
  tMin: number;
  tMax: number;
  currentTick: number;
}) {
  const { width, height } = { width: 460, height: 140 };
  const margin = { top: 8, right: 12, bottom: 18, left: 28 };
  const innerW = width - margin.left - margin.right;
  const innerH = height - margin.top - margin.bottom;

  const yMax = Math.max(
    1,
    d3.max(lambdaPoints, (d) => d.value) ?? 1,
    d3.max(intensityPoints, (d) => d.intensity) ?? 1,
  );

  const xScale = d3.scaleLinear().domain([tMin, tMax]).range([0, innerW]);
  const yScale = d3.scaleLinear().domain([0, yMax]).nice().range([innerH, 0]);

  const lambdaPath = d3
    .line<{ tick: number; value: number }>()
    .x((d) => xScale(d.tick))
    .y((d) => yScale(d.value))
    .curve(d3.curveMonotoneX)(lambdaPoints) ?? '';

  const intensityPath = d3
    .line<IntensityPoint>()
    .x((d) => xScale(d.tick))
    .y((d) => yScale(d.intensity))
    .curve(d3.curveMonotoneX)(intensityPoints) ?? '';

  const xTicks = xScale.ticks(6);
  const yTicks = yScale.ticks(4);

  return (
    <div className="bg-zinc-950 border border-zinc-800 rounded p-2">
      <svg width="100%" height={height} viewBox={`0 0 ${width} ${height}`} preserveAspectRatio="none">
        <g transform={`translate(${margin.left},${margin.top})`}>
          {/* Y grid */}
          {yTicks.map((y) => (
            <g key={y}>
              <line x1={0} x2={innerW} y1={yScale(y)} y2={yScale(y)} stroke="#27272a" strokeDasharray="2,2" />
              <text x={-4} y={yScale(y) + 3} fontSize={9} fill="#71717a" textAnchor="end">{y.toFixed(1)}</text>
            </g>
          ))}
          {/* X axis */}
          {xTicks.map((x) => (
            <g key={x}>
              <line x1={xScale(x)} x2={xScale(x)} y1={innerH} y2={innerH + 3} stroke="#3f3f46" />
              <text x={xScale(x)} y={innerH + 12} fontSize={9} fill="#71717a" textAnchor="middle">{Math.round(x)}</text>
            </g>
          ))}
          <line x1={0} x2={innerW} y1={innerH} y2={innerH} stroke="#3f3f46" />

          {/* current tick marker */}
          {currentTick >= tMin && currentTick <= tMax && (
            <line
              x1={xScale(currentTick)} x2={xScale(currentTick)} y1={0} y2={innerH}
              stroke="#52525b" strokeDasharray="3,3"
            />
          )}

          {/* lambda 理论曲线 */}
          <path d={lambdaPath} fill="none" stroke="#bf4e34" strokeWidth={1.5} strokeDasharray="4,2" />
          {/* I 实测曲线 */}
          <path d={intensityPath} fill="none" stroke="#2e7a91" strokeWidth={1.8} />
        </g>
      </svg>
      <div className="flex gap-3 text-[9px] mt-1">
        <span className="flex items-center gap-1">
          <span style={{ background: '#bf4e34' }} className="inline-block w-3 h-0.5"></span>
          λ(t) theoretical
        </span>
        <span className="flex items-center gap-1">
          <span style={{ background: '#2e7a91' }} className="inline-block w-3 h-0.5"></span>
          I(t) measured
        </span>
      </div>
    </div>
  );
}

function WeightsBar({ weights }: { weights: Record<string, number> }) {
  const entries = Object.entries(weights);
  if (entries.length === 0) {
    return <div className="text-[10px] text-zinc-600 italic">No weights configured.</div>;
  }
  // 渲染 exp(β) 倍率，便于直觉理解（>1 = 偏好，<1 = 抑制，1 = 无差别）。
  const items = entries.map(([tag, beta]) => ({ tag, beta, mult: Math.exp(beta) }));
  const maxMult = Math.max(...items.map((i) => i.mult), 1.5);

  return (
    <div className="flex flex-col gap-1 text-[10px]">
      {items.map((it) => {
        const pct = Math.min(100, (it.mult / maxMult) * 100);
        const positive = it.beta >= 0;
        return (
          <div key={it.tag} className="flex items-center gap-2">
            <span className="font-mono text-zinc-400 w-44 truncate" title={it.tag}>
              {it.tag.replace(/^Npc\./, '')}
            </span>
            <div className="flex-1 h-2 bg-zinc-900 rounded overflow-hidden">
              <div
                className={positive ? 'bg-emerald-500/60 h-full' : 'bg-rose-500/60 h-full'}
                style={{ width: `${pct}%` }}
              />
            </div>
            <span className="font-mono text-zinc-500 w-20 text-right">
              β={it.beta.toFixed(2)} ×{it.mult.toFixed(2)}
            </span>
          </div>
        );
      })}
    </div>
  );
}

function CouplingList({ coupling }: { coupling: Record<string, number> }) {
  const entries = Object.entries(coupling);
  if (entries.length === 0) {
    return <div className="text-[10px] text-zinc-600 italic">No coupling configured.</div>;
  }
  return (
    <div className="flex flex-col gap-0.5 text-[10px] font-mono">
      {entries.map(([other, m]) => (
        <div key={other} className="flex justify-between gap-2">
          <span className="text-zinc-400 truncate">{other}</span>
          <span className={m >= 0 ? 'text-emerald-400' : 'text-rose-400'}>
            {m >= 0 ? '+' : ''}
            {m.toFixed(3)}
          </span>
        </div>
      ))}
    </div>
  );
}
