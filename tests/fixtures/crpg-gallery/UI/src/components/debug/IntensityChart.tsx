import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import * as d3 from 'd3';
import type { RumorRow, IntensityPoint } from '@/stores/rumor-test-store';

interface IntensityChartProps {
  rumors: RumorRow[];
  history: Record<string, IntensityPoint[]>;
  /** Chart 高度（宽度始终自适应容器）。 */
  height?: number;
}

/**
 * Rumor 强度时序折线图（dev 调试用）
 *
 * 每条已激活 rumor 一条线；x = SimTick，y = Intensity。颜色按 d3.schemeCategory10
 * 循环分配。容器宽度变化用 ResizeObserver 自动重绘。
 */
export function IntensityChart({ rumors, history, height = 200 }: IntensityChartProps) {
  const containerRef = useRef<HTMLDivElement>(null);
  const svgRef = useRef<SVGSVGElement>(null);
  const [width, setWidth] = useState(800);

  // 容器宽度自适应——父布局可能 resize（如折叠某栏）。
  useLayoutEffect(() => {
    if (!containerRef.current) return;
    setWidth(containerRef.current.clientWidth);
    const ro = new ResizeObserver((entries) => {
      for (const entry of entries) {
        setWidth(entry.contentRect.width);
      }
    });
    ro.observe(containerRef.current);
    return () => ro.disconnect();
  }, []);

  useEffect(() => {
    const svg = d3.select(svgRef.current);
    svg.selectAll('*').remove();

    // 只画已激活 + 有历史的（变体也算 active）。
    const series = rumors
      .filter((r) => r.active && (history[r.id]?.length ?? 0) > 0)
      .map((r, i) => ({
        id: r.id,
        isBaseline: r.isBaseline,
        isVariant: r.isVariant,
        points: history[r.id] ?? [],
        color: d3.schemeCategory10[i % 10],
      }));

    const margin = { top: 10, right: 12, bottom: 22, left: 32 };
    const innerW = Math.max(0, width - margin.left - margin.right);
    const innerH = Math.max(0, height - margin.top - margin.bottom);

    const root = svg
      .attr('width', width)
      .attr('height', height)
      .append('g')
      .attr('transform', `translate(${margin.left},${margin.top})`);

    // 空状态：placeholder text
    if (series.length === 0) {
      root
        .append('text')
        .attr('x', innerW / 2)
        .attr('y', innerH / 2)
        .attr('text-anchor', 'middle')
        .attr('fill', '#52525b')
        .attr('font-size', 11)
        .attr('font-style', 'italic')
        .text('No active rumors — click Activate to start tracking.');
      return;
    }

    // 全局轴范围。
    const allTicks = series.flatMap((s) => s.points.map((p) => p.tick));
    const allIntensities = series.flatMap((s) => s.points.map((p) => p.intensity));
    const xMin = d3.min(allTicks) ?? 0;
    const xMax = d3.max(allTicks) ?? 1;
    // y 上限至少 1，以免起始全 0 时图被压扁；之后超过时随之扩张。
    const yMax = Math.max(1, d3.max(allIntensities) ?? 1);

    const xScale = d3.scaleLinear()
      .domain([xMin, Math.max(xMax, xMin + 1)])
      .range([0, innerW]);

    const yScale = d3.scaleLinear()
      .domain([0, yMax])
      .nice()
      .range([innerH, 0]);

    // ── 网格 + 坐标轴 ──
    const yAxis = d3.axisLeft(yScale).ticks(4).tickSize(-innerW).tickFormat(d3.format('.1f'));
    const xAxis = d3.axisBottom(xScale).ticks(Math.min(8, xMax - xMin + 1)).tickFormat(d3.format('d'));

    const yAxisG = root.append('g').call(yAxis);
    yAxisG.selectAll('text').attr('fill', '#71717a').attr('font-size', 9);
    yAxisG.selectAll('line').attr('stroke', '#27272a').attr('stroke-dasharray', '2,2');
    yAxisG.select('.domain').remove();

    const xAxisG = root.append('g').attr('transform', `translate(0,${innerH})`).call(xAxis);
    xAxisG.selectAll('text').attr('fill', '#71717a').attr('font-size', 9);
    xAxisG.selectAll('line').attr('stroke', '#3f3f46');
    xAxisG.select('.domain').attr('stroke', '#3f3f46');

    // ── 折线 ──
    const line = d3.line<IntensityPoint>()
      .x((d) => xScale(d.tick))
      .y((d) => yScale(d.intensity))
      .curve(d3.curveMonotoneX);

    for (const s of series) {
      root
        .append('path')
        .datum(s.points)
        .attr('fill', 'none')
        .attr('stroke', s.color)
        .attr('stroke-width', 1.5)
        .attr('d', line);

      // 末端标点——便于读最新值。
      const last = s.points[s.points.length - 1];
      if (last) {
        root
          .append('circle')
          .attr('cx', xScale(last.tick))
          .attr('cy', yScale(last.intensity))
          .attr('r', 2.5)
          .attr('fill', s.color);
      }
    }

    // ── 图例（右上角，鼠标 hover 高亮线条留给后续）──
    const legend = svg
      .append('g')
      .attr('transform', `translate(${margin.left + 4}, ${margin.top + 2})`);

    series.forEach((s, i) => {
      const ly = i * 12;
      legend
        .append('line')
        .attr('x1', 0).attr('y1', ly + 4)
        .attr('x2', 12).attr('y2', ly + 4)
        .attr('stroke', s.color)
        .attr('stroke-width', 1.5);

      const tagPrefix = s.isVariant ? '· ' : s.isBaseline ? '* ' : '';
      legend
        .append('text')
        .attr('x', 16)
        .attr('y', ly + 7)
        .attr('font-size', 9)
        .attr('font-family', 'monospace')
        .attr('fill', '#a1a1aa')
        .text(`${tagPrefix}${s.id}`);
    });
  }, [rumors, history, width, height]);

  return (
    <div ref={containerRef} className="w-full">
      <svg ref={svgRef} />
    </div>
  );
}
