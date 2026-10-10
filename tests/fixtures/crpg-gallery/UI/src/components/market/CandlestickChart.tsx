import { useEffect, useMemo, useRef } from 'react';
import * as d3 from 'd3';
import { useSettingsStore } from '@/stores/settings-store';
import { useMarketPalette } from '@/hooks/use-market-color';
import type { DailyBar } from '@/stores/market-store';

interface Props {
  /** 逐日 OHLC（旧 → 新）。 */
  bars: DailyBar[];
  /** 每根蜡烛聚合多少天。 */
  candleDays: number;
  /**
   * 买卖点。barIdx = 成交日对应的 bars 下标（由调用方换算：day - firstBarDay），shares 正买负卖，
   * price = 成交价（精确点位小圆点的 y）。越界 barIdx 会被忽略。
   */
  trades?: { barIdx: number; shares: number; price: number }[];
}

interface Candle {
  open: number;
  close: number;
  low: number;
  high: number;
  startDay: number;
}

// 涨/跌颜色由玩家「涨跌配色」设置决定（见 useMarketPalette）；其余轴/网格/均线为固定主题色。
const MA5C = '#d4a657';
const MA10C = '#6b8fc2';
const GRID = 'rgba(212,168,67,0.08)';
const AXIS_LINE = 'rgba(212,168,67,0.15)';
const AXIS_TEXT = 'rgba(245,230,200,0.55)';
const CROSS = 'rgba(245,230,200,0.35)';

const W = 1400;
const H = 460;
const M = { t: 22, r: 64, b: 34, l: 14 };
const MIN_VISIBLE = 12; // 最多放大到这么少的蜡烛

function buildCandles(bars: DailyBar[], size: number): Candle[] {
  const out: Candle[] = [];
  const step = Math.max(1, Math.floor(size));
  for (let i = 0; i < bars.length; i += step) {
    const slice = bars.slice(i, i + step);
    if (slice.length === 0) continue;
    // 每根 bar = [open, high, low, close]。桶内聚合：开=首日开，收=末日收，高=各日高最大，低=各日低最小。
    let high = slice[0][1];
    let low = slice[0][2];
    for (const b of slice) {
      if (b[1] > high) high = b[1];
      if (b[2] < low) low = b[2];
    }
    out.push({ open: slice[0][0], close: slice[slice.length - 1][3], low, high, startDay: i });
  }
  return out;
}

/** 全量移动平均，下标对齐蜡烛；不足窗口处为 null。 */
function fullMA(closes: number[], n: number): (number | null)[] {
  const out: (number | null)[] = new Array(closes.length).fill(null);
  for (let i = n - 1; i < closes.length; i++) {
    let sum = 0;
    for (let j = 0; j < n; j++) sum += closes[i - j];
    out[i] = sum / n;
  }
  return out;
}

/**
 * 蜡烛图 —— D3 + SVG（gallery 同款高性能套路）+ 视窗缩放/平移。
 *  · 静态部分按当前可见区间绘制；滚轮以光标为锚缩放（改可见根数），拖拽平移（改起点）；
 *  · y 轴自动贴合可见区间的价格范围；
 *  · 所有指针坐标走 `clientX / uiScale` 修正（WebKit 615 在 CSS zoom 下 clientX 是视觉像素、
 *    getBoundingClientRect 是布局像素，必须先除以 uiScale）。
 */
export function CandlestickChart({ bars, candleDays, trades }: Props) {
  const palette = useMarketPalette();
  const svgRef = useRef<SVGSVGElement>(null);
  const viewRef = useRef<{ start: number; count: number } | null>(null);
  const prevLenRef = useRef(0);
  const candleDaysRef = useRef(candleDays);
  const candles = useMemo(() => buildCandles(bars, candleDays), [bars, candleDays]);

  // 买卖点按蜡烛聚合：三角 = 该蜡烛内有买/有卖（各一个）；小圆点 = 每笔成交的精确价位（不聚合）。
  const marksByCandle = useMemo(() => {
    const step = Math.max(1, Math.floor(candleDays));
    const map = new Map<number, { buy: boolean; sell: boolean; fills: { price: number; buy: boolean }[] }>();
    for (const t of trades ?? []) {
      if (t.barIdx < 0 || t.barIdx >= bars.length) continue;
      const ci = Math.floor(t.barIdx / step);
      const entry = map.get(ci) ?? { buy: false, sell: false, fills: [] };
      if (t.shares > 0) entry.buy = true;
      else if (t.shares < 0) entry.sell = true;
      entry.fills.push({ price: t.price, buy: t.shares > 0 });
      map.set(ci, entry);
    }
    return map;
  }, [trades, bars.length, candleDays]);

  useEffect(() => {
    const node = svgRef.current;
    if (!node) return;
    const UP = palette.upHex;
    const DOWN = palette.downHex;
    const svg = d3.select(node);
    svg.selectAll('*').remove();
    if (candles.length === 0) {
      prevLenRef.current = 0;
      viewRef.current = null;
      return;
    }

    const len = candles.length;
    const closes = candles.map((c) => c.close);
    const ma5 = fullMA(closes, 5);
    const ma10 = fullMA(closes, 10);

    // 初始化 / 夹紧视窗：切粒度(日/周/旬)重置为看全部；推进数据时——原本看全部就继续看全部，
    // 原本看到末尾就跟随到新末尾，否则保持当前窗口。
    const granularityChanged = candleDaysRef.current !== candleDays;
    candleDaysRef.current = candleDays;
    let view = viewRef.current;
    if (!view || granularityChanged) {
      view = { start: 0, count: len };
    } else {
      const prevLen = prevLenRef.current;
      const wasShowingAll = view.start === 0 && view.count >= prevLen;
      if (wasShowingAll) {
        view = { start: 0, count: len };
      } else {
        view.count = Math.min(Math.max(MIN_VISIBLE, view.count), len);
        const wasAtEnd = prevLen > 0 && view.start + view.count >= prevLen;
        if (wasAtEnd) view.start = len - view.count;
        view.start = Math.max(0, Math.min(view.start, len - view.count));
      }
    }
    viewRef.current = view;
    prevLenRef.current = len;

    const chartH = H - M.t - M.b;

    // 持久层（每次 effect 建一次，draw() 内只更新）
    const gGrid = svg.append('g');
    const gCandles = svg.append('g');
    const gMarks = svg.append('g'); // 买卖点（B/S），叠在蜡烛上方
    const pMA5 = svg.append('path').attr('fill', 'none').attr('stroke', MA5C).attr('stroke-width', 1).attr('opacity', 0.85);
    const pMA10 = svg.append('path').attr('fill', 'none').attr('stroke', MA10C).attr('stroke-width', 1).attr('opacity', 0.85);
    const gYAxis = svg.append('g').attr('transform', `translate(${W - M.r},0)`);
    const gXAxis = svg.append('g').attr('transform', `translate(0,${M.t + chartH})`);

    const legend = svg.append('g').attr('font-family', 'monospace');
    legend.append('text').attr('x', M.l + 2).attr('y', 14).attr('fill', MA5C).attr('font-size', '11px').text('— MA5');
    legend.append('text').attr('x', M.l + 58).attr('y', 14).attr('fill', MA10C).attr('font-size', '11px').text('— MA10');
    const readout = svg.append('text').attr('x', M.l + 128).attr('y', 14).attr('font-size', '11px').attr('font-family', 'monospace').attr('display', 'none').style('pointer-events', 'none');

    const cv = svg.append('line').attr('stroke', CROSS).attr('stroke-dasharray', '3,3').attr('display', 'none').style('pointer-events', 'none');
    const ch = svg.append('line').attr('stroke', CROSS).attr('stroke-dasharray', '3,3').attr('display', 'none').style('pointer-events', 'none');
    const priceTag = svg.append('g').attr('display', 'none').style('pointer-events', 'none');
    priceTag.append('rect').attr('width', 50).attr('height', 17).attr('rx', 3).attr('fill', 'rgba(212,168,67,0.92)');
    const priceTxt = priceTag.append('text').attr('x', 25).attr('y', 12).attr('text-anchor', 'middle').attr('fill', '#0a0806').attr('font-size', '11px').attr('font-family', 'monospace');

    const lineGen = d3.line<{ x: number; v: number }>().x((d) => d.x).y((d) => d.v);

    let curXStep = 1;
    let drawState: { x: d3.ScaleBand<number>; y: d3.ScaleLinear<number, number>; visible: Candle[] } | null = null;

    function draw() {
      const v = viewRef.current!;
      const visible = candles.slice(v.start, v.start + v.count);
      if (visible.length === 0) return;

      const x = d3.scaleBand<number>().domain(visible.map((_, i) => i)).range([M.l, W - M.r]).padding(0.3);
      curXStep = x.step();
      let lo = visible[0].low;
      let hi = visible[0].high;
      for (const c of visible) {
        if (c.low < lo) lo = c.low;
        if (c.high > hi) hi = c.high;
      }
      const pad = (hi - lo) * 0.05 || 1;
      const y = d3.scaleLinear().domain([lo - pad, hi + pad]).range([M.t + chartH, M.t]);
      const cx = (i: number) => x(i)! + x.bandwidth() / 2;

      gGrid.selectAll('line').data(y.ticks(6)).join('line')
        .attr('x1', M.l).attr('x2', W - M.r).attr('y1', (d) => y(d)).attr('y2', (d) => y(d)).attr('stroke', GRID);

      gYAxis.call(d3.axisRight(y).ticks(6).tickFormat((d) => (d as number).toFixed(1)) as never)
        .call((s) => {
          s.selectAll('.domain').remove();
          s.selectAll('.tick line').attr('stroke', AXIS_LINE);
          s.selectAll('.tick text').attr('fill', AXIS_TEXT).attr('font-size', '11px').attr('font-family', 'monospace');
        });

      const step = Math.max(1, Math.ceil(visible.length / 8));
      const tickVals = visible.map((_, i) => i).filter((i) => i % step === 0);
      gXAxis.call(d3.axisBottom(x).tickValues(tickVals).tickFormat((i) => `D${visible[i as number].startDay}`) as never)
        .call((s) => {
          s.selectAll('.domain').attr('stroke', AXIS_LINE);
          s.selectAll('.tick line').attr('stroke', AXIS_LINE);
          s.selectAll('.tick text').attr('fill', AXIS_TEXT).attr('font-size', '10px').attr('font-family', 'monospace');
        });

      gCandles.selectAll<SVGLineElement, Candle>('line.wk').data(visible).join('line').attr('class', 'wk')
        .attr('x1', (_, i) => cx(i)).attr('x2', (_, i) => cx(i)).attr('y1', (d) => y(d.high)).attr('y2', (d) => y(d.low))
        .attr('stroke', (d) => (d.close >= d.open ? UP : DOWN)).attr('stroke-width', 1);

      gCandles.selectAll<SVGRectElement, Candle>('rect.bd').data(visible).join('rect').attr('class', 'bd')
        .attr('x', (_, i) => x(i)!).attr('width', x.bandwidth())
        .attr('y', (d) => y(Math.max(d.open, d.close))).attr('height', (d) => Math.max(1, Math.abs(y(d.open) - y(d.close))))
        .attr('fill', (d) => (d.close >= d.open ? UP : DOWN));

      const ma5pts: { x: number; v: number }[] = [];
      const ma10pts: { x: number; v: number }[] = [];
      for (let i = 0; i < visible.length; i++) {
        const g = v.start + i;
        if (ma5[g] != null) ma5pts.push({ x: cx(i), v: y(ma5[g] as number) });
        if (ma10[g] != null) ma10pts.push({ x: cx(i), v: y(ma10[g] as number) });
      }
      pMA5.attr('d', lineGen(ma5pts));
      pMA10.attr('d', lineGen(ma10pts));

      // 买卖点：B = 涨色三角（低点下方，指向蜡烛），S = 跌色倒三角（高点上方）。券商图惯例。
      const marks: { i: number; kind: 'b' | 's'; y: number }[] = [];
      for (let i = 0; i < visible.length; i++) {
        const m = marksByCandle.get(v.start + i);
        if (!m) continue;
        const c = visible[i];
        if (m.buy) marks.push({ i, kind: 'b', y: Math.min(M.t + chartH - 16, y(c.low) + 6) });
        if (m.sell) marks.push({ i, kind: 's', y: Math.max(M.t + 16, y(c.high) - 6) });
      }
      gMarks.selectAll<SVGPathElement, (typeof marks)[number]>('path.tm').data(marks).join('path').attr('class', 'tm')
        .attr('d', (d) => (d.kind === 'b'
          ? `M ${cx(d.i)} ${d.y} l -4.5 7 h 9 z`      // ▲ 指向蜡烛低点
          : `M ${cx(d.i)} ${d.y} l -4.5 -7 h 9 z`))   // ▼ 指向蜡烛高点
        .attr('fill', (d) => (d.kind === 'b' ? UP : DOWN));
      gMarks.selectAll<SVGTextElement, (typeof marks)[number]>('text.tml').data(marks).join('text').attr('class', 'tml')
        .attr('x', (d) => cx(d.i))
        .attr('y', (d) => (d.kind === 'b' ? d.y + 17 : d.y - 10))
        .attr('text-anchor', 'middle').attr('font-size', '9px').attr('font-family', 'monospace')
        .attr('fill', (d) => (d.kind === 'b' ? UP : DOWN))
        .text((d) => (d.kind === 'b' ? 'B' : 'S'));

      // 精确成交点：每笔一个小圆点，y = 成交价（周/旬聚合下同一根蜡烛里的多笔各自可见）。
      // 价位滚出可见 y 域（缩放到别处）时跳过，不画贴边的误导点。
      const dots: { i: number; py: number; buy: boolean }[] = [];
      for (let i = 0; i < visible.length; i++) {
        const m = marksByCandle.get(v.start + i);
        if (!m) continue;
        for (const f of m.fills) {
          const py = y(f.price);
          if (py < M.t || py > M.t + chartH) continue;
          dots.push({ i, py, buy: f.buy });
        }
      }
      gMarks.selectAll<SVGCircleElement, (typeof dots)[number]>('circle.td').data(dots).join('circle').attr('class', 'td')
        .attr('cx', (d) => cx(d.i)).attr('cy', (d) => d.py).attr('r', 2.5)
        .attr('fill', (d) => (d.buy ? UP : DOWN))
        .attr('stroke', 'rgba(245,230,200,0.9)').attr('stroke-width', 0.8);

      drawState = { x, y, visible };
    }

    draw();

    // hover 命中层（置顶）
    const overlay = svg.append('rect').attr('x', M.l).attr('y', M.t).attr('width', W - M.l - M.r).attr('height', chartH)
      .attr('fill', 'transparent').style('cursor', 'crosshair');

    const scaleOf = () => useSettingsStore.getState().uiScale || 1;
    const mxOf = (event: MouseEvent) => {
      const r = node.getBoundingClientRect();
      return ((event.clientX / scaleOf() - r.left) / r.width) * W;
    };
    const myOf = (event: MouseEvent) => {
      const r = node.getBoundingClientRect();
      return ((event.clientY / scaleOf() - r.top) / r.height) * H;
    };

    const hideCursor = () => {
      cv.attr('display', 'none');
      ch.attr('display', 'none');
      priceTag.attr('display', 'none');
      readout.attr('display', 'none');
    };

    let drawRaf = 0;
    const scheduleDraw = () => {
      if (drawRaf) return;
      drawRaf = requestAnimationFrame(() => {
        drawRaf = 0;
        draw();
      });
    };

    let dragging = false;
    let lastDragMx = 0;
    let dragAccum = 0;

    overlay
      .on('mousedown', (event: MouseEvent) => {
        dragging = true;
        lastDragMx = mxOf(event);
        dragAccum = 0;
        hideCursor();
      })
      .on('mousemove', (event: MouseEvent) => {
        const mx = mxOf(event);
        if (dragging) {
          const v = viewRef.current!;
          dragAccum += -(mx - lastDragMx) / curXStep;
          lastDragMx = mx;
          const shift = Math.trunc(dragAccum);
          if (shift !== 0) {
            dragAccum -= shift;
            v.start = Math.max(0, Math.min(v.start + shift, candles.length - v.count));
            scheduleDraw();
          }
          return;
        }
        if (!drawState) return;
        const { x, y, visible } = drawState;
        let li = Math.floor((mx - M.l) / curXStep);
        li = Math.max(0, Math.min(visible.length - 1, li));
        const d = visible[li];
        const cxx = x(li)! + x.bandwidth() / 2;
        const myc = Math.max(M.t, Math.min(M.t + chartH, myOf(event)));

        cv.attr('x1', cxx).attr('x2', cxx).attr('y1', M.t).attr('y2', M.t + chartH).attr('display', null);
        ch.attr('x1', M.l).attr('x2', W - M.r).attr('y1', myc).attr('y2', myc).attr('display', null);
        priceTag.attr('transform', `translate(${W - M.r},${myc - 8.5})`).attr('display', null);
        priceTxt.text(y.invert(myc).toFixed(1));
        const bull = d.close >= d.open;
        const chg = d.open > 0 ? ((d.close - d.open) / d.open) * 100 : 0;
        readout.attr('display', null).attr('fill', bull ? UP : DOWN)
          .text(`D${d.startDay}  O ${d.open.toFixed(1)}  H ${d.high.toFixed(1)}  L ${d.low.toFixed(1)}  C ${d.close.toFixed(1)}  ${bull ? '+' : ''}${chg.toFixed(1)}%`);
      })
      .on('mouseup', () => {
        dragging = false;
      })
      .on('mouseleave', () => {
        dragging = false;
        hideCursor();
      });

    // 拖拽时鼠标松开在图外也要结束
    const winUp = () => {
      dragging = false;
    };
    window.addEventListener('mouseup', winUp);

    // 滚轮缩放（native + passive:false 才能 preventDefault 阻止页面滚动）
    const wheelHandler = (event: WheelEvent) => {
      event.preventDefault();
      const mx = mxOf(event);
      const frac = Math.max(0, Math.min(1, (mx - M.l) / (W - M.l - M.r)));
      const v = viewRef.current!;
      const factor = event.deltaY < 0 ? 0.85 : 1 / 0.85; // 上滚放大
      let newCount = Math.round(v.count * factor);
      newCount = Math.max(MIN_VISIBLE, Math.min(candles.length, newCount));
      const anchor = v.start + frac * v.count;
      let newStart = Math.round(anchor - frac * newCount);
      newStart = Math.max(0, Math.min(newStart, candles.length - newCount));
      viewRef.current = { start: newStart, count: newCount };
      scheduleDraw();
    };
    // 用 d3 .on 而非 node.addEventListener(..., {passive:false})：后者的 options-对象形式在 Ultralight
    // 下可能有问题（旧版能跑的图表全走 d3 .on）。element 上的 wheel 默认非 passive，preventDefault 仍生效。
    svg.on('wheel.zoom', (event) => wheelHandler(event as WheelEvent));

    return () => {
      cancelAnimationFrame(drawRaf);
      svg.on('wheel.zoom', null);
      window.removeEventListener('mouseup', winUp);
    };
  }, [candles, marksByCandle, palette.upHex, palette.downHex]);

  return (
    <div className="relative w-full" style={{ height: 440 }}>
      <svg ref={svgRef} viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none" className="w-full h-full block" />
      {candles.length === 0 && (
        <div className="absolute inset-0 flex items-center justify-center text-sm text-zinc-600 italic">
          暂无历史数据 —— 用下方「推进」按钮或 MarketSimulateDays N 推进市场。
        </div>
      )}
    </div>
  );
}
