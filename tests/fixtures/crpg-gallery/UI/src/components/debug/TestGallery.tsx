import { useState, useCallback, useEffect, useRef } from 'react';
import { sendToUE } from '@/lib/ue-bridge';
import * as d3 from 'd3';
import * as THREE from 'three';
import { motion, AnimatePresence } from 'framer-motion';
import { animated, useSpring, useTrail } from '@react-spring/web';
import { Sword, Shield, Heart, Flame, Star, Zap, Crown, Skull, Gem, ScrollText } from 'lucide-react';
import { animate, stagger, utils, createTimeline, createDraggable } from 'animejs';
import lottie from 'lottie-web';

// ═══════════════════════════════════════════════════════════
// CSS Feature Tests
// ═══════════════════════════════════════════════════════════

interface TestCell {
  label: string;
  size?: 'normal' | 'large' | 'wide';
  render: () => React.ReactNode;
}

// ── D3 Components ──────────────────────────────────────────

function D3BarChart() {
  const ref = useRef<SVGSVGElement>(null);
  useEffect(() => {
    if (!ref.current) return;
    const data = [40, 80, 55, 95, 30, 70, 60, 85, 45, 75];
    const svg = d3.select(ref.current);
    svg.selectAll('*').remove();
    const w = 280, h = 180, margin = { t: 10, r: 10, b: 20, l: 30 };
    const x = d3.scaleBand().domain(data.map((_, i) => String(i))).range([margin.l, w - margin.r]).padding(0.3);
    const y = d3.scaleLinear().domain([0, 100]).range([h - margin.b, margin.t]);
    svg.attr('width', w).attr('height', h);
    svg.selectAll('rect').data(data).join('rect')
      .attr('x', (_, i) => x(String(i))!)
      .attr('width', x.bandwidth())
      .attr('y', d => y(d))
      .attr('height', d => y(0) - y(d))
      .attr('fill', '#d4a843')
      .attr('rx', 2);
    svg.append('g').attr('transform', `translate(0,${h - margin.b})`).call(d3.axisBottom(x).tickSize(0).tickFormat(() => ''));
    svg.append('g').attr('transform', `translate(${margin.l},0)`).call(d3.axisLeft(y).ticks(4).tickSize(-w + margin.l + margin.r));
    svg.selectAll('.tick line').attr('stroke', 'rgba(212,168,67,0.15)');
    svg.selectAll('.domain').attr('stroke', 'rgba(212,168,67,0.3)');
    svg.selectAll('.tick text').attr('fill', 'rgba(245,230,200,0.4)').attr('font-size', '9px');
  }, []);
  return <svg ref={ref} />;
}

function D3LineChart() {
  const ref = useRef<SVGSVGElement>(null);
  useEffect(() => {
    if (!ref.current) return;
    const data = Array.from({ length: 30 }, (_, i) => ({
      x: i, y: 50 + Math.sin(i * 0.5) * 30 + Math.random() * 10,
    }));
    const svg = d3.select(ref.current);
    svg.selectAll('*').remove();
    const w = 280, h = 180, margin = { t: 10, r: 10, b: 20, l: 30 };
    const x = d3.scaleLinear().domain([0, 29]).range([margin.l, w - margin.r]);
    const y = d3.scaleLinear().domain([0, 100]).range([h - margin.b, margin.t]);
    svg.attr('width', w).attr('height', h);

    // Area
    const area = d3.area<{ x: number; y: number }>().x(d => x(d.x)).y0(h - margin.b).y1(d => y(d.y)).curve(d3.curveCatmullRom);
    svg.append('path').datum(data).attr('d', area).attr('fill', 'rgba(212,168,67,0.15)');

    // Line
    const line = d3.line<{ x: number; y: number }>().x(d => x(d.x)).y(d => y(d.y)).curve(d3.curveCatmullRom);
    svg.append('path').datum(data).attr('d', line).attr('fill', 'none').attr('stroke', '#d4a843').attr('stroke-width', 2);

    // Dots
    svg.selectAll('circle').data(data.filter((_, i) => i % 5 === 0)).join('circle')
      .attr('cx', d => x(d.x)).attr('cy', d => y(d.y)).attr('r', 3).attr('fill', '#f0d080');

    svg.append('g').attr('transform', `translate(${margin.l},0)`).call(d3.axisLeft(y).ticks(4).tickSize(-w + margin.l + margin.r));
    svg.selectAll('.tick line').attr('stroke', 'rgba(212,168,67,0.1)');
    svg.selectAll('.domain').attr('stroke', 'rgba(212,168,67,0.3)');
    svg.selectAll('.tick text').attr('fill', 'rgba(245,230,200,0.4)').attr('font-size', '9px');
  }, []);
  return <svg ref={ref} />;
}

function D3PieChart() {
  const ref = useRef<SVGSVGElement>(null);
  useEffect(() => {
    if (!ref.current) return;
    const data = [35, 25, 20, 12, 8];
    const colors = ['#d4a843', '#8b1a1a', '#2a4a8a', '#2d8b2d', '#b8860b'];
    const svg = d3.select(ref.current);
    svg.selectAll('*').remove();
    const w = 200, h = 200, radius = 80;
    svg.attr('width', w).attr('height', h);
    const g = svg.append('g').attr('transform', `translate(${w / 2},${h / 2})`);
    const pie = d3.pie<number>().sort(null);
    const arc = d3.arc<d3.PieArcDatum<number>>().innerRadius(35).outerRadius(radius);
    g.selectAll('path').data(pie(data)).join('path')
      .attr('d', arc)
      .attr('fill', (_, i) => colors[i])
      .attr('stroke', '#0a0806')
      .attr('stroke-width', 2);
  }, []);
  return <svg ref={ref} />;
}

function D3RadarChart() {
  const ref = useRef<SVGSVGElement>(null);
  useEffect(() => {
    if (!ref.current) return;
    const stats = [
      { label: 'STR', value: 0.8 },
      { label: 'DEX', value: 0.6 },
      { label: 'INT', value: 0.9 },
      { label: 'WIS', value: 0.7 },
      { label: 'CHA', value: 0.5 },
      { label: 'CON', value: 0.75 },
    ];
    const svg = d3.select(ref.current);
    svg.selectAll('*').remove();
    const w = 200, h = 200, cx = w / 2, cy = h / 2, r = 75;
    svg.attr('width', w).attr('height', h);

    const angleSlice = (Math.PI * 2) / stats.length;

    // Grid rings
    [0.25, 0.5, 0.75, 1].forEach(level => {
      const points = stats.map((_, i) => {
        const a = angleSlice * i - Math.PI / 2;
        return `${cx + r * level * Math.cos(a)},${cy + r * level * Math.sin(a)}`;
      }).join(' ');
      svg.append('polygon').attr('points', points).attr('fill', 'none').attr('stroke', 'rgba(212,168,67,0.15)');
    });

    // Axes
    stats.forEach((s, i) => {
      const a = angleSlice * i - Math.PI / 2;
      svg.append('line').attr('x1', cx).attr('y1', cy).attr('x2', cx + r * Math.cos(a)).attr('y2', cy + r * Math.sin(a)).attr('stroke', 'rgba(212,168,67,0.2)');
      svg.append('text').attr('x', cx + (r + 14) * Math.cos(a)).attr('y', cy + (r + 14) * Math.sin(a))
        .attr('text-anchor', 'middle').attr('dominant-baseline', 'central')
        .attr('fill', 'rgba(245,230,200,0.5)').attr('font-size', '10px').text(s.label);
    });

    // Data polygon
    const dataPoints = stats.map((s, i) => {
      const a = angleSlice * i - Math.PI / 2;
      return `${cx + r * s.value * Math.cos(a)},${cy + r * s.value * Math.sin(a)}`;
    }).join(' ');
    svg.append('polygon').attr('points', dataPoints).attr('fill', 'rgba(212,168,67,0.2)').attr('stroke', '#d4a843').attr('stroke-width', 2);

    // Data dots
    stats.forEach((s, i) => {
      const a = angleSlice * i - Math.PI / 2;
      svg.append('circle').attr('cx', cx + r * s.value * Math.cos(a)).attr('cy', cy + r * s.value * Math.sin(a))
        .attr('r', 3).attr('fill', '#f0d080');
    });
  }, []);
  return <svg ref={ref} />;
}

function D3AnimatedBars() {
  const ref = useRef<SVGSVGElement>(null);
  useEffect(() => {
    if (!ref.current) return;
    const svg = d3.select(ref.current);
    svg.selectAll('*').remove();
    const w = 280, h = 180;
    svg.attr('width', w).attr('height', h);

    function update() {
      const data = Array.from({ length: 8 }, () => Math.random() * 90 + 10);
      const x = d3.scaleBand().domain(data.map((_, i) => String(i))).range([20, w - 10]).padding(0.2);
      const y = d3.scaleLinear().domain([0, 100]).range([h - 10, 10]);
      svg.selectAll('rect').data(data).join('rect')
        .transition().duration(800).ease(d3.easeCubicOut)
        .attr('x', (_, i) => x(String(i))!)
        .attr('width', x.bandwidth())
        .attr('y', d => y(d))
        .attr('height', d => y(0) - y(d))
        .attr('fill', '#d4a843')
        .attr('rx', 2);
    }
    update();
    const interval = setInterval(update, 1500);
    return () => clearInterval(interval);
  }, []);
  return <svg ref={ref} />;
}

function D3ForceGraph() {
  const ref = useRef<SVGSVGElement>(null);
  useEffect(() => {
    if (!ref.current) return;
    const svg = d3.select(ref.current);
    svg.selectAll('*').remove();
    const w = 280, h = 200;
    svg.attr('width', w).attr('height', h);

    const nodes = Array.from({ length: 15 }, (_, i) => ({ id: i, r: 4 + Math.random() * 6 }));
    const links = Array.from({ length: 20 }, () => ({
      source: Math.floor(Math.random() * 15),
      target: Math.floor(Math.random() * 15),
    })).filter(l => l.source !== l.target);

    const sim = d3.forceSimulation(nodes as any)
      .force('link', d3.forceLink(links as any).distance(40))
      .force('charge', d3.forceManyBody().strength(-30))
      .force('center', d3.forceCenter(w / 2, h / 2));

    const linkEl = svg.selectAll('line').data(links).join('line')
      .attr('stroke', 'rgba(212,168,67,0.3)').attr('stroke-width', 1);
    const nodeEl = svg.selectAll('circle').data(nodes).join('circle')
      .attr('r', d => (d as any).r).attr('fill', '#d4a843');

    sim.on('tick', () => {
      linkEl.attr('x1', (d: any) => d.source.x).attr('y1', (d: any) => d.source.y)
        .attr('x2', (d: any) => d.target.x).attr('y2', (d: any) => d.target.y);
      nodeEl.attr('cx', (d: any) => d.x).attr('cy', (d: any) => d.y);
    });

    return () => { sim.stop(); };
  }, []);
  return <svg ref={ref} />;
}

function D3Heatmap() {
  const ref = useRef<SVGSVGElement>(null);
  useEffect(() => {
    if (!ref.current) return;
    const svg = d3.select(ref.current);
    svg.selectAll('*').remove();
    const rows = 8, cols = 12, cellW = 22, cellH = 22, margin = 5;
    const w = cols * cellW + margin * 2, h = rows * cellH + margin * 2;
    svg.attr('width', w).attr('height', h);
    const color = d3.scaleSequential(d3.interpolateYlOrBr).domain([0, 1]);
    for (let r = 0; r < rows; r++) {
      for (let c = 0; c < cols; c++) {
        svg.append('rect')
          .attr('x', margin + c * cellW).attr('y', margin + r * cellH)
          .attr('width', cellW - 2).attr('height', cellH - 2)
          .attr('rx', 2)
          .attr('fill', color(Math.random()));
      }
    }
  }, []);
  return <svg ref={ref} />;
}

function D3GaugeChart() {
  const ref = useRef<SVGSVGElement>(null);
  useEffect(() => {
    if (!ref.current) return;
    const svg = d3.select(ref.current);
    svg.selectAll('*').remove();
    const w = 200, h = 120, cx = w / 2, cy = h - 10, r = 80;
    svg.attr('width', w).attr('height', h);

    const bg = d3.arc<any>().innerRadius(r - 15).outerRadius(r).startAngle(-Math.PI / 2).endAngle(Math.PI / 2);
    const fg = d3.arc<any>().innerRadius(r - 15).outerRadius(r).startAngle(-Math.PI / 2).endAngle(-Math.PI / 2 + Math.PI * 0.72);

    svg.append('path').attr('d', bg({} as any)).attr('transform', `translate(${cx},${cy})`).attr('fill', 'rgba(212,168,67,0.15)');
    svg.append('path').attr('d', fg({} as any)).attr('transform', `translate(${cx},${cy})`).attr('fill', '#d4a843');
    svg.append('text').attr('x', cx).attr('y', cy - 20).attr('text-anchor', 'middle')
      .attr('fill', '#f0d080').attr('font-size', '24px').attr('font-weight', 'bold').text('72%');
    svg.append('text').attr('x', cx).attr('y', cy - 2).attr('text-anchor', 'middle')
      .attr('fill', 'rgba(245,230,200,0.4)').attr('font-size', '10px').text('HEALTH');
  }, []);
  return <svg ref={ref} />;
}

// ═══════════════════════════════════════════════════════════
// Test Grid
// ═══════════════════════════════════════════════════════════

const CSS_TESTS: TestCell[] = [
  // ── Static rendering ──
  { label: 'Solid Color', render: () => <div className="w-full h-full bg-gold" /> },
  { label: 'Linear Gradient', render: () => <div className="w-full h-full" style={{ background: 'linear-gradient(135deg, #d4a843, #8b1a1a)' }} /> },
  { label: 'Radial Gradient', render: () => <div className="w-full h-full" style={{ background: 'radial-gradient(circle, #d4a843 0%, transparent 70%)' }} /> },
  { label: 'Conic Gradient ✗', render: () => <div className="w-full h-full" style={{ background: 'conic-gradient(from 0deg, #d4a843, #8b1a1a, #2a4a8a, #d4a843)' }} /> },

  // ── Hover / Transition ──
  { label: 'Hover Scale', render: () => {
    const [h, setH] = useState(false);
    return <div className="w-20 h-20 mx-auto mt-6 rounded-lg bg-gold cursor-pointer transition-transform duration-300" style={{ transform: h ? 'scale(1.4)' : 'scale(1)' }} onMouseEnter={() => setH(true)} onMouseLeave={() => setH(false)} />;
  }},
  { label: 'Hover Color + BG', render: () => {
    const [h, setH] = useState(false);
    return <div className="mx-6 mt-6 px-6 py-4 rounded border-l-3 cursor-pointer transition-all duration-400" style={{ borderColor: h ? '#d4a843' : 'transparent', backgroundColor: h ? 'rgba(212,168,67,0.15)' : 'transparent', color: h ? '#f0d080' : '#f5e6c8' }} onMouseEnter={() => setH(true)} onMouseLeave={() => setH(false)}>
      <div className="text-lg font-bold">Menu Item</div>
      <div className="text-xs opacity-50">Hover me</div>
    </div>;
  }},
  { label: 'Multi-property Transition', render: () => {
    const [h, setH] = useState(false);
    return <div className="w-16 h-16 mx-auto mt-6 cursor-pointer transition-all duration-500" style={{ backgroundColor: h ? '#d4a843' : '#2a4a8a', borderRadius: h ? '50%' : '4px', transform: h ? 'rotate(180deg) scale(1.2)' : 'rotate(0) scale(1)' }} onMouseEnter={() => setH(true)} onMouseLeave={() => setH(false)} />;
  }},
  { label: 'Width Transition', render: () => {
    const [h, setH] = useState(false);
    return <div className="mx-6 mt-10 cursor-pointer" onClick={() => setH(!h)}>
      <div className="h-3 rounded-full bg-gold/30 overflow-hidden"><div className="h-full bg-gold rounded-full transition-all duration-1000 ease-out" style={{ width: h ? '100%' : '20%' }} /></div>
      <div className="text-xs text-parchment/40 mt-2 text-center">Click to toggle</div>
    </div>;
  }},

  // ── Keyframe Animations: Game UI ──
  { label: 'HP Bar Drain/Heal', render: () => (
    <div className="mx-6 mt-8">
      <div className="text-xs text-parchment/40 mb-2">Health</div>
      <div className="h-5 bg-black/50 rounded-full overflow-hidden border border-gold/20">
        <div className="h-full rounded-full animate-hp-drain" />
      </div>
    </div>
  )},
  { label: 'Damage Shake', render: () => (
    <div className="flex items-center justify-center h-full">
      <div className="w-24 h-24 bg-gold/20 border-2 border-gold rounded-lg animate-shake flex items-center justify-center">
        <span className="text-gold text-2xl font-bold">-42</span>
      </div>
    </div>
  )},
  { label: 'Damage Flash Overlay', render: () => (
    <div className="relative w-full h-full">
      <div className="absolute inset-0 flex items-center justify-center text-parchment text-sm">Game World</div>
      <div className="absolute inset-0 animate-damage-flash pointer-events-none" />
    </div>
  )},
  { label: 'Loot Bounce In', render: () => (
    <div className="flex items-center justify-center h-full gap-4">
      {['🗡️', '🛡️', '💎'].map((e, i) => (
        <div key={i} className="animate-bounce-in text-3xl" style={{ animationDelay: `${i * 0.2}s` }}>{e}</div>
      ))}
    </div>
  )},
  { label: 'Float / Bob', render: () => (
    <div className="flex items-center justify-center h-full gap-6">
      <div className="w-10 h-10 bg-gold rounded-full animate-float-bob" />
      <div className="w-10 h-10 bg-mana rounded-full animate-float-bob" style={{ animationDelay: '0.5s' }} />
      <div className="w-10 h-10 bg-blood rounded-full animate-float-bob" style={{ animationDelay: '1s' }} />
    </div>
  )},
  { label: 'Swing (Pendulum)', render: () => (
    <div className="flex items-center justify-center h-full">
      <div className="w-4 h-24 bg-gold rounded animate-swing origin-top" />
    </div>
  )},

  // ── Keyframe Animations: UI Elements ──
  { label: 'Slide In (Left + Right)', render: () => (
    <div className="flex flex-col gap-3 mx-6 mt-4">
      <div className="px-4 py-2 bg-gold/20 rounded animate-slide-in-l text-parchment text-sm border-l-2 border-gold">← From Left</div>
      <div className="px-4 py-2 bg-mana/20 rounded animate-slide-in-r text-parchment text-sm border-r-2 border-mana" style={{ animationDelay: '0.3s' }}>From Right →</div>
      <div className="px-4 py-2 bg-blood/20 rounded animate-slide-up text-parchment text-sm border-b-2 border-blood" style={{ animationDelay: '0.6s' }}>↑ From Bottom</div>
    </div>
  )},
  { label: 'Staggered List', render: () => (
    <div className="flex flex-col gap-2 mx-6 mt-4">
      {['Strength +2', 'Dexterity +1', 'Intelligence +3', 'Wisdom +1', 'Charisma +2'].map((s, i) => (
        <div key={i} className="px-3 py-1.5 bg-gold/10 border-l-2 border-gold text-parchment text-sm animate-stagger-fade" style={{ animationDelay: `${i * 0.12}s` }}>{s}</div>
      ))}
    </div>
  )},
  { label: 'Color Cycle Text', render: () => (
    <div className="flex items-center justify-center h-full">
      <span className="text-2xl font-bold animate-color-cycle">LEGENDARY</span>
    </div>
  )},
  { label: 'Border Chase', render: () => (
    <div className="flex items-center justify-center h-full">
      <div className="w-24 h-24 border-4 rounded-lg animate-border-chase bg-transparent" />
    </div>
  )},
  { label: 'Gradient Shift BG', render: () => (
    <div className="w-full h-full animate-gradient-shift" style={{ background: 'linear-gradient(90deg, #d4a843, #8b1a1a, #2a4a8a, #d4a843)', backgroundSize: '300% 100%' }} />
  )},
  { label: 'Typewriter + Caret', render: () => (
    <div className="flex items-center justify-center h-full">
      <span className="text-lg font-mono text-gold overflow-hidden whitespace-nowrap border-r-2 border-gold animate-typewriter animate-blink-caret" style={{ animation: 'typewriter 3s steps(20) infinite alternate, blink-caret 0.8s step-end infinite' }}>
        Quest Complete!
      </span>
    </div>
  )},
  { label: 'Striped Progress Bar', render: () => (
    <div className="mx-6 mt-10">
      <div className="text-xs text-parchment/40 mb-2">Loading...</div>
      <div className="h-4 rounded-full overflow-hidden bg-black/50 border border-gold/20">
        <div className="h-full w-[65%] rounded-full animate-progress-stripe" style={{ backgroundImage: 'linear-gradient(45deg, rgba(255,255,255,0.1) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.1) 50%, rgba(255,255,255,0.1) 75%, transparent 75%)', backgroundSize: '40px 40px', backgroundColor: '#d4a843' }} />
      </div>
    </div>
  )},
  { label: 'Flip Card', render: () => (
    <div className="flex items-center justify-center h-full">
      <div className="w-20 h-28 animate-flip-x" style={{ transformStyle: 'preserve-3d' }}>
        <div className="absolute inset-0 bg-gold rounded-lg flex items-center justify-center text-black font-bold text-xl">?</div>
      </div>
    </div>
  )},
  { label: 'Ripple Effect', render: () => (
    <div className="relative flex items-center justify-center h-full">
      <div className="w-12 h-12 bg-gold rounded-full" />
      <div className="absolute w-12 h-12 rounded-full border-2 border-gold animate-ripple" />
      <div className="absolute w-12 h-12 rounded-full border-2 border-gold animate-ripple" style={{ animationDelay: '0.5s' }} />
    </div>
  )},
  { label: 'Pulse Ring', render: () => (
    <div className="relative flex items-center justify-center h-full">
      <div className="w-10 h-10 bg-gold rounded-full z-10" />
      {[0, 1, 2].map(i => (
        <div key={i} className="absolute w-10 h-10 rounded-full border border-gold/50 animate-ripple" style={{ animationDelay: `${i * 0.5}s`, animationDuration: '2s' }} />
      ))}
    </div>
  )},
  { label: 'Spin + Pulse Combo', render: () => (
    <div className="flex items-center justify-center h-full">
      <div className="w-16 h-16 border-4 border-t-gold border-r-gold/30 border-b-gold/10 border-l-gold/30 rounded-full animate-spin" style={{ animationDuration: '1.5s' }} />
    </div>
  )},
  { label: 'Multi-element Orchestra', render: () => (
    <div className="flex items-end justify-center h-full gap-1 pb-8">
      {Array.from({ length: 12 }, (_, i) => (
        <div key={i} className="w-3 bg-gold rounded-t transition-all" style={{ height: `${20 + Math.sin(i * 0.8) * 30 + 30}%`, animation: `float-bob ${1.5 + (i % 3) * 0.5}s ease-in-out infinite`, animationDelay: `${i * 0.1}s`, opacity: 0.5 + (i % 3) * 0.2 }} />
      ))}
    </div>
  )},
];

const D3_TESTS: TestCell[] = [
  { label: 'D3 Bar Chart', size: 'large', render: () => <D3BarChart /> },
  { label: 'D3 Line + Area', size: 'large', render: () => <D3LineChart /> },
  { label: 'D3 Donut Chart', size: 'large', render: () => <D3PieChart /> },
  { label: 'D3 Radar (RPG Stats)', size: 'large', render: () => <D3RadarChart /> },
  { label: 'D3 Animated Bars', size: 'large', render: () => <D3AnimatedBars /> },
  { label: 'D3 Force Graph', size: 'large', render: () => <D3ForceGraph /> },
  { label: 'D3 Heatmap', size: 'large', render: () => <D3Heatmap /> },
  { label: 'D3 Gauge', size: 'large', render: () => <D3GaugeChart /> },
];

// ── D3 Interactive (Full-width) ────────────────────────────

function D3Sunburst() {
  const ref = useRef<SVGSVGElement>(null);
  const tooltipRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!ref.current) return;
    const svg = d3.select(ref.current);
    svg.selectAll('*').remove();
    const w = 800, h = 800, radius = w / 2;
    svg.attr('viewBox', `0 0 ${w} ${h}`).attr('width', '100%').attr('height', '100%');
    const g = svg.append('g').attr('transform', `translate(${w/2},${h/2})`);

    const data = {
      name: 'Game', children: [
        { name: 'Combat', children: [
          { name: 'Melee', value: 30 }, { name: 'Ranged', value: 20 },
          { name: 'Magic', children: [
            { name: 'Fire', value: 15 }, { name: 'Ice', value: 10 }, { name: 'Lightning', value: 8 },
          ]},
        ]},
        { name: 'Exploration', children: [
          { name: 'Dungeons', value: 25 }, { name: 'Towns', value: 18 }, { name: 'Wilderness', value: 22 },
        ]},
        { name: 'Social', children: [
          { name: 'Dialogue', value: 20 }, { name: 'Trade', value: 12 }, { name: 'Quests', value: 28 },
        ]},
        { name: 'Crafting', children: [
          { name: 'Alchemy', value: 14 }, { name: 'Smithing', value: 16 }, { name: 'Enchanting', value: 10 },
        ]},
      ],
    };

    const root = d3.hierarchy(data).sum(d => (d as any).value).sort((a, b) => (b.value || 0) - (a.value || 0));
    const partition = d3.partition<typeof data>().size([2 * Math.PI, radius]);
    partition(root as any);

    const color = d3.scaleOrdinal(d3.schemeSet2);
    const arc = d3.arc<any>()
      .startAngle(d => d.x0).endAngle(d => d.x1)
      .innerRadius(d => d.y0 * 0.8).outerRadius(d => d.y1 * 0.8 - 1);

    const paths = g.selectAll('path').data(root.descendants().filter(d => d.depth)).join('path')
      .attr('d', arc)
      .attr('fill', d => { let node = d; while (node.depth > 1) node = node.parent!; return color(node.data.name); })
      .attr('fill-opacity', d => 1 - d.depth * 0.15)
      .attr('stroke', '#0a0806').attr('stroke-width', 1)
      .attr('cursor', 'pointer');

    // Hover interaction
    paths.on('mouseenter', function(_event, d) {
      d3.select(this).attr('fill-opacity', 1).attr('stroke', '#f0d080').attr('stroke-width', 2);
      if (tooltipRef.current) {
        tooltipRef.current.style.display = 'block';
        tooltipRef.current.textContent = `${d.data.name}: ${d.value}`;
      }
    }).on('mousemove', function(event) {
      if (tooltipRef.current) {
        const [mx, my] = d3.pointer(event, ref.current);
        tooltipRef.current.style.left = `${mx + 15}px`;
        tooltipRef.current.style.top = `${my - 10}px`;
      }
    }).on('mouseleave', function(_, d) {
      d3.select(this).attr('fill-opacity', 1 - d.depth * 0.15).attr('stroke', '#0a0806').attr('stroke-width', 1);
      if (tooltipRef.current) tooltipRef.current.style.display = 'none';
    }).on('click', function(_, d) {
      sendToUE('sunburst_click', { name: d.data.name, value: d.value, depth: d.depth });
    });

    // Center labels
    g.selectAll('text').data(root.descendants().filter(d => d.depth && ((d as any).y1 - (d as any).y0) * ((d as any).x1 - (d as any).x0) > 0.04)).join('text')
      .attr('transform', d => {
        const x = ((d as any).x0 + (d as any).x1) / 2 * 180 / Math.PI;
        const y = ((d as any).y0 + (d as any).y1) / 2 * 0.8;
        return `rotate(${x - 90}) translate(${y},0) rotate(${x < 180 ? 0 : 180})`;
      })
      .attr('text-anchor', 'middle').attr('font-size', '9px').attr('fill', 'rgba(245,230,200,0.8)')
      .text(d => d.data.name);
  }, []);
  return (
    <div className="relative w-full h-full">
      <svg ref={ref} className="w-full h-full" />
      <div ref={tooltipRef} className="absolute hidden px-2 py-1 bg-black/80 text-gold text-xs rounded pointer-events-none border border-gold/30" />
    </div>
  );
}

function D3InteractiveTree() {
  const ref = useRef<SVGSVGElement>(null);
  const [collapsed, setCollapsed] = useState<Set<string>>(new Set());
  useEffect(() => {
    if (!ref.current) return;
    const svg = d3.select(ref.current);
    svg.selectAll('*').remove();
    const w = 900, h = 600, margin = { t: 20, r: 40, b: 20, l: 100 };
    svg.attr('viewBox', `0 0 ${w} ${h}`).attr('width', '100%').attr('height', '100%');

    const data = {
      name: 'Character', children: [
        { name: 'Attributes', children: [
          { name: 'Strength' }, { name: 'Dexterity' }, { name: 'Intelligence' }, { name: 'Wisdom' },
        ]},
        { name: 'Skills', children: [
          { name: 'Swordsmanship' }, { name: 'Archery' }, { name: 'Stealth' },
          { name: 'Magic', children: [{ name: 'Fireball' }, { name: 'Heal' }, { name: 'Shield' }] },
        ]},
        { name: 'Inventory', children: [
          { name: 'Weapons' }, { name: 'Armor' }, { name: 'Potions' },
        ]},
      ],
    };

    // Filter collapsed
    function filterTree(node: any): any {
      if (collapsed.has(node.name) || !node.children) return { ...node, children: undefined };
      return { ...node, children: node.children.map(filterTree) };
    }
    const filtered = filterTree(data);
    const root = d3.hierarchy(filtered);
    const tree = d3.tree().size([h - margin.t - margin.b, w - margin.l - margin.r]);
    tree(root as any);

    const g = svg.append('g').attr('transform', `translate(${margin.l},${margin.t})`);

    // Links
    g.selectAll('path.link').data(root.links()).join('path')
      .attr('class', 'link')
      .attr('d', d3.linkHorizontal<any, any>().x(d => d.y).y(d => d.x) as any)
      .attr('fill', 'none').attr('stroke', 'rgba(212,168,67,0.3)').attr('stroke-width', 1.5);

    // Nodes
    const node = g.selectAll('g.node').data(root.descendants()).join('g')
      .attr('class', 'node')
      .attr('transform', (d: any) => `translate(${d.y},${d.x})`)
      .attr('cursor', 'pointer');

    node.append('circle').attr('r', (d: any) => d.children || collapsed.has(d.data.name) ? 6 : 4)
      .attr('fill', (d: any) => collapsed.has(d.data.name) ? '#8b1a1a' : d.children ? '#d4a843' : '#5c4033')
      .attr('stroke', '#d4a843').attr('stroke-width', 1.5);

    node.append('text')
      .attr('x', (d: any) => d.children || collapsed.has(d.data.name) ? -10 : 10)
      .attr('dy', 4)
      .attr('text-anchor', (d: any) => d.children || collapsed.has(d.data.name) ? 'end' : 'start')
      .attr('font-size', '11px').attr('fill', '#f5e6c8')
      .text((d: any) => d.data.name);

    node.on('click', function(_, d: any) {
      setCollapsed(prev => {
        const next = new Set(prev);
        if (next.has(d.data.name)) next.delete(d.data.name); else next.add(d.data.name);
        return next;
      });
      sendToUE('tree_click', { name: d.data.name });
    });

    node.on('mouseenter', function() {
      d3.select(this).select('circle').attr('stroke-width', 3).attr('stroke', '#f0d080');
    }).on('mouseleave', function() {
      d3.select(this).select('circle').attr('stroke-width', 1.5).attr('stroke', '#d4a843');
    });
  }, [collapsed]);
  return <svg ref={ref} />;
}

function D3BrushableTimeline() {
  const ref = useRef<SVGSVGElement>(null);
  const [selection, setSelection] = useState<[number, number] | null>(null);
  useEffect(() => {
    if (!ref.current) return;
    const svg = d3.select(ref.current);
    svg.selectAll('*').remove();
    const w = 900, h = 300, margin = { t: 20, r: 20, b: 30, l: 50 };
    svg.attr('viewBox', `0 0 ${w} ${h}`).attr('width', '100%').attr('height', '100%');

    const data = Array.from({ length: 50 }, (_, i) => ({
      day: i, events: Math.floor(Math.random() * 20) + 1,
    }));

    const x = d3.scaleLinear().domain([0, 49]).range([margin.l, w - margin.r]);
    const y = d3.scaleLinear().domain([0, 25]).range([h - margin.b, margin.t]);

    const g = svg.append('g');

    // Bars
    const barW = (w - margin.l - margin.r) / data.length - 1;
    g.selectAll('rect').data(data).join('rect')
      .attr('x', d => x(d.day) - barW / 2)
      .attr('width', barW)
      .attr('y', d => y(d.events))
      .attr('height', d => y(0) - y(d.events))
      .attr('fill', '#d4a843')
      .attr('opacity', 0.7);

    // Axes
    svg.append('g').attr('transform', `translate(0,${h - margin.b})`).call(d3.axisBottom(x).ticks(10).tickFormat(d => `D${d}`));
    svg.append('g').attr('transform', `translate(${margin.l},0)`).call(d3.axisLeft(y).ticks(5));
    svg.selectAll('.tick line').attr('stroke', 'rgba(212,168,67,0.15)');
    svg.selectAll('.domain').attr('stroke', 'rgba(212,168,67,0.3)');
    svg.selectAll('.tick text').attr('fill', 'rgba(245,230,200,0.4)').attr('font-size', '9px');

    // Brush
    const brush = d3.brushX()
      .extent([[margin.l, margin.t], [w - margin.r, h - margin.b]])
      .on('end', (event) => {
        if (!event.selection) { setSelection(null); return; }
        const [x0, x1] = event.selection as [number, number];
        const d0 = Math.round(x.invert(x0));
        const d1 = Math.round(x.invert(x1));
        setSelection([d0, d1]);
        sendToUE('timeline_brush', { from: d0, to: d1 });

        // Highlight selected bars
        g.selectAll('rect').attr('opacity', (d: any) => d.day >= d0 && d.day <= d1 ? 1 : 0.2);
      });

    svg.append('g').attr('class', 'brush').call(brush);
    svg.selectAll('.selection').attr('fill', 'rgba(212,168,67,0.2)').attr('stroke', '#d4a843');
  }, []);
  return (
    <div>
      <svg ref={ref} />
      {selection && (
        <div className="text-xs text-gold/60 text-center mt-1">
          Selected: Day {selection[0]} – Day {selection[1]}
        </div>
      )}
    </div>
  );
}

function D3ZoomableScatter() {
  const ref = useRef<SVGSVGElement>(null);
  useEffect(() => {
    if (!ref.current) return;
    const svg = d3.select(ref.current);
    svg.selectAll('*').remove();
    const w = 900, h = 700, margin = { t: 20, r: 20, b: 30, l: 50 };
    svg.attr('viewBox', `0 0 ${w} ${h}`).attr('width', '100%').attr('height', '100%');

    const data = Array.from({ length: 80 }, () => ({
      x: Math.random() * 100, y: Math.random() * 100,
      r: 3 + Math.random() * 8,
      type: Math.random() > 0.5 ? 'warrior' : 'mage',
    }));

    const x = d3.scaleLinear().domain([0, 100]).range([margin.l, w - margin.r]);
    const y = d3.scaleLinear().domain([0, 100]).range([h - margin.b, margin.t]);

    svg.append('defs').append('clipPath').attr('id', 'scatter-clip')
      .append('rect').attr('x', margin.l).attr('y', margin.t)
      .attr('width', w - margin.l - margin.r).attr('height', h - margin.t - margin.b);

    const g = svg.append('g').attr('clip-path', 'url(#scatter-clip)');

    const xAxis = svg.append('g').attr('transform', `translate(0,${h - margin.b})`);
    const yAxis = svg.append('g').attr('transform', `translate(${margin.l},0)`);

    function updateAxes(newX: any, newY: any) {
      xAxis.call(d3.axisBottom(newX).ticks(8));
      yAxis.call(d3.axisLeft(newY).ticks(8));
      svg.selectAll('.tick line').attr('stroke', 'rgba(212,168,67,0.15)');
      svg.selectAll('.domain').attr('stroke', 'rgba(212,168,67,0.3)');
      svg.selectAll('.tick text').attr('fill', 'rgba(245,230,200,0.4)').attr('font-size', '9px');
    }

    const dots = g.selectAll('circle').data(data).join('circle')
      .attr('cx', d => x(d.x)).attr('cy', d => y(d.y)).attr('r', d => d.r)
      .attr('fill', d => d.type === 'warrior' ? '#d4a843' : '#4a7acc')
      .attr('fill-opacity', 0.7)
      .attr('stroke', d => d.type === 'warrior' ? '#f0d080' : '#7ab0ff')
      .attr('stroke-width', 1)
      .attr('cursor', 'pointer');

    dots.on('mouseenter', function() {
      d3.select(this).attr('fill-opacity', 1).attr('stroke-width', 2);
    }).on('mouseleave', function() {
      d3.select(this).attr('fill-opacity', 0.7).attr('stroke-width', 1);
    }).on('click', function(_, d) {
      sendToUE('scatter_click', { x: d.x.toFixed(1), y: d.y.toFixed(1), type: d.type });
    });

    updateAxes(x, y);

    // Zoom
    const zoom = d3.zoom<SVGSVGElement, unknown>()
      .scaleExtent([0.5, 10])
      .on('zoom', (event) => {
        const newX = event.transform.rescaleX(x);
        const newY = event.transform.rescaleY(y);
        dots.attr('cx', (d: any) => newX(d.x)).attr('cy', (d: any) => newY(d.y));
        updateAxes(newX, newY);
      });

    svg.call(zoom);
  }, []);
  return <svg ref={ref} />;
}

const D3_INTERACTIVE_TESTS: TestCell[] = [
  { label: 'Sunburst (hover + click + tooltip)', render: () => <D3Sunburst /> },
  { label: 'Collapsible Skill Tree (click to toggle)', render: () => <D3InteractiveTree /> },
  { label: 'Brushable Timeline (drag to select)', render: () => <D3BrushableTimeline /> },
  { label: 'Zoomable Scatter (scroll to zoom, hover, click)', render: () => <D3ZoomableScatter /> },
];

// ── Canvas 2D Particle System ──────────────────────────────

function Canvas2DParticles() {
  const ref = useRef<HTMLCanvasElement>(null);
  useEffect(() => {
    const canvas = ref.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d')!;
    if (!ctx) return;
    const W = 800, H = 600;
    canvas.width = W; canvas.height = H;

    interface Particle { x: number; y: number; vx: number; vy: number; life: number; maxLife: number; size: number; hue: number; }
    const particles: Particle[] = [];
    let mouse = { x: W / 2, y: H / 2 };
    let animId = 0;

    canvas.onmousemove = (e) => {
      const rect = canvas.getBoundingClientRect();
      mouse.x = (e.clientX - rect.left) / rect.width * W;
      mouse.y = (e.clientY - rect.top) / rect.height * H;
    };

    function spawn(count: number) {
      for (let i = 0; i < count; i++) {
        const angle = Math.random() * Math.PI * 2;
        const speed = 0.5 + Math.random() * 2;
        particles.push({
          x: mouse.x, y: mouse.y,
          vx: Math.cos(angle) * speed, vy: Math.sin(angle) * speed - 1,
          life: 0, maxLife: 60 + Math.random() * 60,
          size: 2 + Math.random() * 4,
          hue: 30 + Math.random() * 30, // gold range
        });
      }
    }

    function loop() {
      ctx.fillStyle = 'rgba(10,8,6,0.15)';
      ctx.fillRect(0, 0, W, H);
      spawn(3);

      for (let i = particles.length - 1; i >= 0; i--) {
        const p = particles[i];
        p.x += p.vx; p.y += p.vy;
        p.vy += 0.02; // gravity
        p.life++;
        const alpha = 1 - p.life / p.maxLife;
        if (alpha <= 0) { particles.splice(i, 1); continue; }
        ctx.beginPath();
        ctx.arc(p.x, p.y, p.size * alpha, 0, Math.PI * 2);
        ctx.fillStyle = `hsla(${p.hue}, 70%, 60%, ${alpha})`;
        ctx.fill();
      }
      animId = requestAnimationFrame(loop);
    }
    loop();
    return () => cancelAnimationFrame(animId);
  }, []);
  return <canvas ref={ref} className="w-full h-full" style={{ background: '#0a0806' }} />;
}

function Canvas2DFireworks() {
  const ref = useRef<HTMLCanvasElement>(null);
  useEffect(() => {
    const canvas = ref.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d')!;
    if (!ctx) return;
    const W = 800, H = 600;
    canvas.width = W; canvas.height = H;

    interface Spark { x: number; y: number; vx: number; vy: number; life: number; color: string; }
    const sparks: Spark[] = [];
    let animId = 0;

    function explode(x: number, y: number) {
      const count = 40 + Math.floor(Math.random() * 30);
      const hue = Math.random() * 360;
      for (let i = 0; i < count; i++) {
        const angle = (i / count) * Math.PI * 2 + Math.random() * 0.3;
        const speed = 2 + Math.random() * 4;
        sparks.push({
          x, y, vx: Math.cos(angle) * speed, vy: Math.sin(angle) * speed,
          life: 60 + Math.random() * 30,
          color: `hsl(${hue + Math.random() * 40}, 80%, ${50 + Math.random() * 30}%)`,
        });
      }
    }

    let frame = 0;
    function loop() {
      ctx.fillStyle = 'rgba(10,8,6,0.12)';
      ctx.fillRect(0, 0, W, H);
      frame++;
      if (frame % 60 === 0) explode(100 + Math.random() * (W - 200), 100 + Math.random() * (H / 2));

      for (let i = sparks.length - 1; i >= 0; i--) {
        const s = sparks[i];
        s.x += s.vx; s.y += s.vy;
        s.vy += 0.05;
        s.vx *= 0.98; s.vy *= 0.98;
        s.life--;
        if (s.life <= 0) { sparks.splice(i, 1); continue; }
        const alpha = s.life / 90;
        ctx.beginPath();
        ctx.arc(s.x, s.y, 2, 0, Math.PI * 2);
        ctx.fillStyle = s.color.replace(')', `, ${alpha})`).replace('hsl', 'hsla');
        ctx.fill();
      }
      animId = requestAnimationFrame(loop);
    }
    loop();
    canvas.onclick = (e) => {
      const rect = canvas.getBoundingClientRect();
      explode((e.clientX - rect.left) / rect.width * W, (e.clientY - rect.top) / rect.height * H);
    };
    return () => cancelAnimationFrame(animId);
  }, []);
  return <canvas ref={ref} className="w-full h-full cursor-crosshair" style={{ background: '#0a0806' }} />;
}

// ── Three.js WebGL ─────────────────────────────────────────

function ThreeJSCube() {
  const ref = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!ref.current) return;
    const W = 800, H = 600;
    const scene = new THREE.Scene();
    const camera = new THREE.PerspectiveCamera(60, W / H, 0.1, 100);
    camera.position.z = 3;

    let renderer: THREE.WebGLRenderer;
    try {
      renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
    } catch {
      ref.current.innerHTML = '<div class="text-red-400 text-center mt-20">WebGL not available</div>';
      return;
    }
    renderer.setSize(W, H);
    renderer.setClearColor(0x000000, 0);
    ref.current.appendChild(renderer.domElement);
    renderer.domElement.style.width = '100%';
    renderer.domElement.style.height = '100%';

    // Gold wireframe cube
    const geo = new THREE.BoxGeometry(1.5, 1.5, 1.5);
    const mat = new THREE.MeshBasicMaterial({ color: 0xd4a843, wireframe: true });
    const cube = new THREE.Mesh(geo, mat);
    scene.add(cube);

    // Particle cloud around it
    const pGeo = new THREE.BufferGeometry();
    const positions = new Float32Array(300 * 3);
    for (let i = 0; i < 300; i++) {
      positions[i * 3] = (Math.random() - 0.5) * 5;
      positions[i * 3 + 1] = (Math.random() - 0.5) * 5;
      positions[i * 3 + 2] = (Math.random() - 0.5) * 5;
    }
    pGeo.setAttribute('position', new THREE.BufferAttribute(positions, 3));
    const pMat = new THREE.PointsMaterial({ color: 0xf0d080, size: 0.03 });
    const points = new THREE.Points(pGeo, pMat);
    scene.add(points);

    let animId = 0;
    function animate() {
      cube.rotation.x += 0.008;
      cube.rotation.y += 0.012;
      points.rotation.y -= 0.003;
      renderer.render(scene, camera);
      animId = requestAnimationFrame(animate);
    }
    animate();

    return () => {
      cancelAnimationFrame(animId);
      renderer.dispose();
      renderer.domElement.remove();
    };
  }, []);
  return <div ref={ref} className="w-full h-full" />;
}

function ThreeJSParticleField() {
  const ref = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!ref.current) return;
    const W = 800, H = 600;
    const scene = new THREE.Scene();
    const camera = new THREE.PerspectiveCamera(75, W / H, 0.1, 100);
    camera.position.z = 30;

    let renderer: THREE.WebGLRenderer;
    try {
      renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
    } catch {
      ref.current.innerHTML = '<div class="text-red-400 text-center mt-20">WebGL not available</div>';
      return;
    }
    renderer.setSize(W, H);
    renderer.setClearColor(0x0a0806, 1);
    ref.current.appendChild(renderer.domElement);
    renderer.domElement.style.width = '100%';
    renderer.domElement.style.height = '100%';

    // 2000 particles in a sphere
    const count = 2000;
    const geo = new THREE.BufferGeometry();
    const pos = new Float32Array(count * 3);
    const colors = new Float32Array(count * 3);
    for (let i = 0; i < count; i++) {
      const r = 15 + Math.random() * 10;
      const theta = Math.random() * Math.PI * 2;
      const phi = Math.acos(2 * Math.random() - 1);
      pos[i * 3] = r * Math.sin(phi) * Math.cos(theta);
      pos[i * 3 + 1] = r * Math.sin(phi) * Math.sin(theta);
      pos[i * 3 + 2] = r * Math.cos(phi);
      // Gold to red gradient based on distance
      const t = Math.random();
      colors[i * 3] = 0.83 * (1 - t) + 0.55 * t; // R
      colors[i * 3 + 1] = 0.66 * (1 - t) + 0.1 * t; // G
      colors[i * 3 + 2] = 0.26 * (1 - t) + 0.1 * t; // B
    }
    geo.setAttribute('position', new THREE.BufferAttribute(pos, 3));
    geo.setAttribute('color', new THREE.BufferAttribute(colors, 3));
    const mat = new THREE.PointsMaterial({ size: 0.15, vertexColors: true, transparent: true, opacity: 0.8 });
    const particles = new THREE.Points(geo, mat);
    scene.add(particles);

    let animId = 0;
    let time = 0;
    function animate() {
      time += 0.005;
      particles.rotation.y = time;
      particles.rotation.x = Math.sin(time * 0.5) * 0.3;

      // Wave distortion
      const positions = geo.attributes.position.array as Float32Array;
      for (let i = 0; i < count; i++) {
        const ix = i * 3;
        const ox = pos[ix], oy = pos[ix + 1], oz = pos[ix + 2];
        const wave = Math.sin(time * 2 + ox * 0.1 + oy * 0.1) * 0.5;
        positions[ix] = ox + wave;
        positions[ix + 1] = oy + wave * 0.5;
        positions[ix + 2] = oz;
      }
      geo.attributes.position.needsUpdate = true;

      renderer.render(scene, camera);
      animId = requestAnimationFrame(animate);
    }
    animate();

    return () => {
      cancelAnimationFrame(animId);
      renderer.dispose();
      renderer.domElement.remove();
    };
  }, []);
  return <div ref={ref} className="w-full h-full" />;
}

// ── D3 Candlestick / K-Line Chart ──────────────────────────

function generateOHLC(count: number) {
  const data: { date: number; open: number; high: number; low: number; close: number; volume: number }[] = [];
  let price = 100 + Math.random() * 50;
  for (let i = 0; i < count; i++) {
    const change = (Math.random() - 0.48) * 4;
    const open = price;
    const close = price + change;
    const high = Math.max(open, close) + Math.random() * 2;
    const low = Math.min(open, close) - Math.random() * 2;
    const volume = 500 + Math.random() * 2000;
    data.push({ date: i, open, high, low, close, volume });
    price = close;
  }
  return data;
}

function D3CandlestickChart() {
  const svgRef = useRef<SVGSVGElement>(null);
  const tooltipRef = useRef<HTMLDivElement>(null);
  const [data] = useState(() => generateOHLC(120));

  useEffect(() => {
    if (!svgRef.current) return;
    const svg = d3.select(svgRef.current);
    svg.selectAll('*').remove();

    const W = 1200, H = 700;
    const margin = { t: 30, r: 60, b: 140, l: 70 };
    const chartH = H - margin.t - margin.b - 100; // main chart height
    const volH = 80; // volume chart height
    const volTop = margin.t + chartH + 20;

    svg.attr('viewBox', `0 0 ${W} ${H}`).attr('width', '100%').attr('height', '100%');

    const x = d3.scaleBand<number>().domain(data.map(d => d.date)).range([margin.l, W - margin.r]).padding(0.3);
    const y = d3.scaleLinear().domain([d3.min(data, d => d.low)! - 2, d3.max(data, d => d.high)! + 2]).range([margin.t + chartH, margin.t]);
    const yVol = d3.scaleLinear().domain([0, d3.max(data, d => d.volume)!]).range([volTop + volH, volTop]);

    const g = svg.append('g');

    // Grid lines
    const yTicks = y.ticks(8);
    g.selectAll('.grid-line').data(yTicks).join('line')
      .attr('x1', margin.l).attr('x2', W - margin.r)
      .attr('y1', d => y(d)).attr('y2', d => y(d))
      .attr('stroke', 'rgba(212,168,67,0.08)');

    // Price axis
    svg.append('g').attr('transform', `translate(${W - margin.r},0)`)
      .call(d3.axisRight(y).ticks(8).tickFormat(d => `$${(d as number).toFixed(1)}`))
      .call(g => { g.selectAll('.domain').remove(); g.selectAll('.tick line').attr('stroke', 'rgba(212,168,67,0.15)'); g.selectAll('.tick text').attr('fill', 'rgba(245,230,200,0.5)').attr('font-size', '11px'); });

    // Time axis (every 10th bar)
    const xAxis = svg.append('g').attr('transform', `translate(0,${margin.t + chartH})`);
    xAxis.call(d3.axisBottom(x).tickValues(data.filter((_, i) => i % 10 === 0).map(d => d.date)).tickFormat(d => `Day ${d}`))
      .call(g => { g.selectAll('.domain').attr('stroke', 'rgba(212,168,67,0.2)'); g.selectAll('.tick line').attr('stroke', 'rgba(212,168,67,0.1)'); g.selectAll('.tick text').attr('fill', 'rgba(245,230,200,0.4)').attr('font-size', '10px'); });

    // Candlestick wicks (high-low lines)
    g.selectAll('.wick').data(data).join('line')
      .attr('class', 'wick')
      .attr('x1', d => x(d.date)! + x.bandwidth() / 2)
      .attr('x2', d => x(d.date)! + x.bandwidth() / 2)
      .attr('y1', d => y(d.high))
      .attr('y2', d => y(d.low))
      .attr('stroke', d => d.close >= d.open ? '#4caf4c' : '#c44040')
      .attr('stroke-width', 1);

    // Candlestick bodies (open-close rects)
    g.selectAll('.candle').data(data).join('rect')
      .attr('class', 'candle')
      .attr('x', d => x(d.date)!)
      .attr('width', x.bandwidth())
      .attr('y', d => y(Math.max(d.open, d.close)))
      .attr('height', d => Math.max(1, Math.abs(y(d.open) - y(d.close))))
      .attr('fill', d => d.close >= d.open ? '#4caf4c' : '#c44040')
      .attr('stroke', d => d.close >= d.open ? '#6dd06d' : '#e05050')
      .attr('stroke-width', 0.5);

    // Volume bars
    g.selectAll('.vol').data(data).join('rect')
      .attr('class', 'vol')
      .attr('x', d => x(d.date)!)
      .attr('width', x.bandwidth())
      .attr('y', d => yVol(d.volume))
      .attr('height', d => yVol(0) - yVol(d.volume))
      .attr('fill', d => d.close >= d.open ? 'rgba(76,175,76,0.4)' : 'rgba(196,64,64,0.4)');

    // Volume axis
    svg.append('g').attr('transform', `translate(${W - margin.r},0)`)
      .call(d3.axisRight(yVol).ticks(3).tickFormat(d => `${((d as number) / 1000).toFixed(1)}K`))
      .call(g => { g.selectAll('.domain').remove(); g.selectAll('.tick line').remove(); g.selectAll('.tick text').attr('fill', 'rgba(245,230,200,0.3)').attr('font-size', '9px'); });

    svg.append('text').attr('x', margin.l + 5).attr('y', volTop - 5).attr('fill', 'rgba(245,230,200,0.3)').attr('font-size', '10px').text('Volume');

    // Moving average (20-period SMA)
    const smaData = data.map((d, i) => {
      if (i < 19) return null;
      const slice = data.slice(i - 19, i + 1);
      return { date: d.date, avg: d3.mean(slice, s => s.close)! };
    }).filter(Boolean) as { date: number; avg: number }[];

    const smaLine = d3.line<{ date: number; avg: number }>()
      .x(d => x(d.date)! + x.bandwidth() / 2)
      .y(d => y(d.avg))
      .curve(d3.curveCatmullRom);

    g.append('path').datum(smaData).attr('d', smaLine)
      .attr('fill', 'none').attr('stroke', '#f0d080').attr('stroke-width', 1.5)
      .attr('stroke-dasharray', '4,2').attr('opacity', 0.7);

    svg.append('text').attr('x', margin.l + 5).attr('y', margin.t - 10).attr('fill', '#f0d080').attr('font-size', '11px').text('SMA(20)');

    // Crosshair
    const crosshairV = svg.append('line').attr('stroke', 'rgba(245,230,200,0.3)').attr('stroke-dasharray', '3,3').attr('display', 'none');
    const crosshairH = svg.append('line').attr('stroke', 'rgba(245,230,200,0.3)').attr('stroke-dasharray', '3,3').attr('display', 'none');
    const priceTag = svg.append('g').attr('display', 'none');
    priceTag.append('rect').attr('width', 60).attr('height', 20).attr('rx', 3).attr('fill', 'rgba(212,168,67,0.9)');
    priceTag.append('text').attr('x', 30).attr('y', 14).attr('text-anchor', 'middle').attr('fill', '#0a0806').attr('font-size', '11px').attr('font-weight', 'bold');

    // Hover overlay
    svg.append('rect')
      .attr('x', margin.l).attr('y', margin.t)
      .attr('width', W - margin.l - margin.r).attr('height', chartH + volH + 20)
      .attr('fill', 'transparent')
      .on('mousemove', function(event) {
        const [mx, my] = d3.pointer(event);
        const bandwidth = x.bandwidth() + x.paddingInner() * x.step();
        const idx = Math.floor((mx - margin.l) / bandwidth);
        if (idx < 0 || idx >= data.length) return;
        const d = data[idx];
        const cx = x(d.date)! + x.bandwidth() / 2;

        crosshairV.attr('x1', cx).attr('x2', cx).attr('y1', margin.t).attr('y2', volTop + volH).attr('display', null);
        crosshairH.attr('x1', margin.l).attr('x2', W - margin.r).attr('y1', my).attr('y2', my).attr('display', null);

        const price = y.invert(my);
        priceTag.attr('transform', `translate(${W - margin.r},${my - 10})`).attr('display', null);
        priceTag.select('text').text(`$${price.toFixed(1)}`);

        if (tooltipRef.current) {
          const bull = d.close >= d.open;
          tooltipRef.current.style.display = 'block';
          tooltipRef.current.style.left = `${cx > W / 2 ? cx - 180 : cx + 20}px`;
          tooltipRef.current.style.top = `${Math.max(margin.t, my - 60)}px`;
          tooltipRef.current.innerHTML = `
            <div style="color:${bull ? '#4caf4c' : '#c44040'};font-weight:bold;margin-bottom:4px">Day ${d.date} ${bull ? '▲' : '▼'}</div>
            <div>Open: <b>$${d.open.toFixed(2)}</b></div>
            <div>High: <b>$${d.high.toFixed(2)}</b></div>
            <div>Low: <b>$${d.low.toFixed(2)}</b></div>
            <div>Close: <b style="color:${bull ? '#4caf4c' : '#c44040'}">$${d.close.toFixed(2)}</b></div>
            <div>Vol: <b>${d.volume.toFixed(0)}</b></div>
            <div style="color:${bull ? '#4caf4c' : '#c44040'}">Change: ${((d.close - d.open) / d.open * 100).toFixed(2)}%</div>
          `;
        }
      })
      .on('mouseleave', function() {
        crosshairV.attr('display', 'none');
        crosshairH.attr('display', 'none');
        priceTag.attr('display', 'none');
        if (tooltipRef.current) tooltipRef.current.style.display = 'none';
      })
      .on('click', function(event) {
        const [mx] = d3.pointer(event);
        const bandwidth = x.bandwidth() + x.paddingInner() * x.step();
        const idx = Math.floor((mx - margin.l) / bandwidth);
        if (idx >= 0 && idx < data.length) {
          sendToUE('kline_click', { day: data[idx].date, close: data[idx].close.toFixed(2) });
        }
      });

    // Title
    svg.append('text').attr('x', W / 2).attr('y', margin.t - 10).attr('text-anchor', 'middle')
      .attr('fill', '#d4a843').attr('font-size', '14px').attr('font-weight', 'bold')
      .text('CRPG Gold Price — 120 Day K-Line');

  }, [data]);

  return (
    <div className="relative w-full h-full">
      <svg ref={svgRef} className="w-full h-full" />
      <div ref={tooltipRef}
        className="absolute hidden px-3 py-2 bg-black/90 text-parchment text-xs rounded border border-gold/30 pointer-events-none leading-relaxed"
        style={{ minWidth: '150px' }}
      />
    </div>
  );
}

const KLINE_TESTS: TestCell[] = [
  { label: 'Interactive K-Line Chart (120 days, OHLC + Volume + SMA + Crosshair + Tooltip)', render: () => <D3CandlestickChart /> },
];

const WEBGL_TESTS: TestCell[] = [
  { label: 'Canvas2D: Mouse Particle Emitter (move mouse)', render: () => <Canvas2DParticles /> },
  { label: 'Canvas2D: Fireworks (click to spawn)', render: () => <Canvas2DFireworks /> },
  { label: 'Three.js: Wireframe Cube + Particle Cloud', render: () => <ThreeJSCube /> },
  { label: 'Three.js: 2000 Particle Sphere Wave', render: () => <ThreeJSParticleField /> },
];

// ── Framer Motion Components ───────────────────────────────

function FramerFadeIn() {
  return (
    <motion.div
      animate={{ opacity: [0, 1, 1, 0], y: [20, 0, 0, 20] }}
      transition={{ duration: 2.5, repeat: Infinity, ease: 'easeInOut' }}
      className="text-gold text-xl font-bold text-center mt-12"
    >
      Fade In Loop
    </motion.div>
  );
}

function FramerSpring() {
  const [flip, setFlip] = useState(false);
  return (
    <div className="flex items-center justify-center h-full cursor-pointer" onClick={() => setFlip(!flip)}>
      <motion.div
        animate={{ rotate: flip ? 180 : 0, scale: flip ? 1.2 : 1 }}
        transition={{ type: 'spring', stiffness: 200, damping: 15 }}
        className="w-16 h-16 bg-gold rounded-lg"
      />
    </div>
  );
}

function FramerGesture() {
  return (
    <div className="flex items-center justify-center h-full">
      <motion.div
        whileHover={{ scale: 1.3, backgroundColor: '#f0d080' }}
        whileTap={{ scale: 0.9 }}
        className="w-20 h-20 bg-gold rounded-full cursor-pointer flex items-center justify-center text-black font-bold"
      >
        TAP
      </motion.div>
    </div>
  );
}

function FramerLayoutAnim() {
  const [expanded, setExpanded] = useState(false);
  return (
    <div className="flex items-center justify-center h-full cursor-pointer" onClick={() => setExpanded(!expanded)}>
      <motion.div
        layout
        className="bg-gold rounded-lg"
        style={{ width: expanded ? 160 : 60, height: expanded ? 80 : 60 }}
        transition={{ type: 'spring', stiffness: 300, damping: 25 }}
      />
    </div>
  );
}

function FramerStagger() {
  const items = ['STR', 'DEX', 'INT', 'WIS'];
  return (
    <div className="flex flex-col gap-2 p-4">
      {items.map((item, i) => (
        <motion.div
          key={item}
          initial={{ opacity: 0, x: -30 }}
          animate={{ opacity: 1, x: 0 }}
          transition={{ delay: i * 0.15, duration: 0.4 }}
          className="px-3 py-1 bg-gold/20 border-l-2 border-gold text-parchment text-sm"
        >
          {item}
        </motion.div>
      ))}
    </div>
  );
}

function FramerPresence() {
  const [show, setShow] = useState(true);
  useEffect(() => {
    const t = setInterval(() => setShow(s => !s), 2000);
    return () => clearInterval(t);
  }, []);
  return (
    <div className="flex items-center justify-center h-full">
      <AnimatePresence>
        {show && (
          <motion.div
            initial={{ opacity: 0, scale: 0.5 }}
            animate={{ opacity: 1, scale: 1 }}
            exit={{ opacity: 0, scale: 0.5 }}
            className="w-16 h-16 bg-gold rounded-full"
          />
        )}
      </AnimatePresence>
    </div>
  );
}

// ── React Spring Components ────────────────────────────────

function SpringNumber() {
  const props = useSpring({
    from: { val: 0 },
    to: { val: 8750 },
    loop: { reverse: true },
    config: { duration: 2000 },
  });
  return (
    <div className="flex items-center justify-center h-full">
      <animated.span className="text-3xl font-bold text-gold font-mono">
        {props.val.to(v => Math.floor(v).toLocaleString())}
      </animated.span>
    </div>
  );
}

function SpringTrail() {
  const items = ['⚔️', '🛡️', '💎', '🔥', '⭐'];
  const trail = useTrail(items.length, {
    from: { opacity: 0, transform: 'translateY(20px)' },
    to: { opacity: 1, transform: 'translateY(0px)' },
    loop: { reverse: true },
    config: { tension: 200, friction: 20 },
  });
  return (
    <div className="flex items-center justify-center h-full gap-3">
      {trail.map((style, i) => (
        <animated.div key={i} style={style} className="text-2xl">
          {items[i]}
        </animated.div>
      ))}
    </div>
  );
}

function SpringProgressBar() {
  const props = useSpring({ width: '75%', from: { width: '0%' }, config: { duration: 1500 } });
  return (
    <div className="flex flex-col items-center justify-center h-full gap-3 px-6">
      <div className="w-full h-4 bg-black/50 rounded-full overflow-hidden border border-gold/30">
        <animated.div style={props} className="h-full bg-gold rounded-full" />
      </div>
      <span className="text-xs text-parchment/40">HP 75/100</span>
    </div>
  );
}

// ── Lucide Icons ───────────────────────────────────────────

function LucideIconGrid() {
  const icons = [
    { Icon: Sword, label: 'Sword' },
    { Icon: Shield, label: 'Shield' },
    { Icon: Heart, label: 'Heart' },
    { Icon: Flame, label: 'Flame' },
    { Icon: Star, label: 'Star' },
    { Icon: Zap, label: 'Zap' },
    { Icon: Crown, label: 'Crown' },
    { Icon: Skull, label: 'Skull' },
    { Icon: Gem, label: 'Gem' },
    { Icon: ScrollText, label: 'Scroll' },
  ];
  return (
    <div className="grid grid-cols-5 gap-3 p-3">
      {icons.map(({ Icon, label }) => (
        <div key={label} className="flex flex-col items-center gap-1">
          <Icon className="w-6 h-6 text-gold" />
          <span className="text-[8px] text-parchment/40">{label}</span>
        </div>
      ))}
    </div>
  );
}

function LucideAnimatedIcons() {
  const [active, setActive] = useState(0);
  useEffect(() => {
    const t = setInterval(() => setActive(a => (a + 1) % 4), 1000);
    return () => clearInterval(t);
  }, []);
  const icons = [Sword, Shield, Heart, Flame];
  return (
    <div className="flex items-center justify-center h-full gap-4">
      {icons.map((Icon, i) => (
        <motion.div key={i} animate={{ scale: active === i ? 1.4 : 1, opacity: active === i ? 1 : 0.3 }}
          transition={{ type: 'spring', stiffness: 300 }}>
          <Icon className="w-8 h-8 text-gold" />
        </motion.div>
      ))}
    </div>
  );
}

// ═══════════════════════════════════════════════════════════
// Anime.js Tests
// ═══════════════════════════════════════════════════════════

function AnimeStaggerGrid() {
  const containerRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!containerRef.current) return;
    const els = containerRef.current.querySelectorAll('.cell');
    const anim = animate(els, {
      scale: [0, 1],
      rotate: ['-180deg', '0deg'],
      backgroundColor: ['#d4a843', '#8b1a1a', '#2a4a8a', '#d4a843'],
      borderRadius: ['0%', '50%', '0%'],
      duration: 2000,
      delay: stagger(50, { grid: [5, 5], from: 'center' }),
      loop: true,
      easing: 'easeInOutQuad',
    });
    return () => { anim.pause(); };
  }, []);

  return (
    <div ref={containerRef} className="grid grid-cols-5 gap-2 p-4 place-items-center h-full content-center">
      {Array.from({ length: 25 }, (_, i) => (
        <div key={i} className="cell w-8 h-8 bg-gold" />
      ))}
    </div>
  );
}

function AnimeTextReveal() {
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!ref.current) return;
    const chars = ref.current.querySelectorAll('.char');
    const anim = animate(chars, {
      opacity: [0, 1],
      translateY: [20, 0],
      delay: stagger(60),
      duration: 600,
      loop: true,
      loopDelay: 1500,
      easing: 'easeOutExpo',
    });
    return () => { anim.pause(); };
  }, []);

  const text = 'Purple Scheme';
  return (
    <div ref={ref} className="flex items-center justify-center h-full">
      <div className="text-2xl font-display font-bold text-gold">
        {text.split('').map((ch, i) => (
          <span key={i} className="char inline-block" style={{ opacity: 0 }}>
            {ch === ' ' ? '\u00A0' : ch}
          </span>
        ))}
      </div>
    </div>
  );
}

function AnimeTimeline() {
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!ref.current) return;
    const box = ref.current.querySelector('.box') as HTMLElement;
    if (!box) return;
    const tl = createTimeline({ loop: true, defaults: { duration: 500 } });
    tl.add(box, { translateX: 200 })
      .add(box, { translateY: 80 })
      .add(box, { translateX: 0, rotate: '360deg' })
      .add(box, { translateY: 0, borderRadius: ['0%', '50%'] })
      .add(box, { borderRadius: ['50%', '0%'], scale: [1.2, 1] });
    return () => { tl.pause(); };
  }, []);

  return (
    <div ref={ref} className="relative h-full p-4">
      <div className="box w-10 h-10 bg-gold rounded" />
    </div>
  );
}

function AnimeElasticBounce() {
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!ref.current) return;
    const balls = ref.current.querySelectorAll('.ball');
    const anim = animate(balls, {
      translateY: [-60, 0],
      scale: [0.8, 1],
      delay: stagger(120),
      duration: 1200,
      loop: true,
      easing: 'spring(1, 80, 10, 0)',
    });
    return () => { anim.pause(); };
  }, []);

  const colors = ['#d4a843', '#c44040', '#4a7acc', '#4caf4c', '#b8860b'];
  return (
    <div ref={ref} className="flex items-end justify-center gap-4 h-full pb-8">
      {colors.map((c, i) => (
        <div key={i} className="ball w-8 h-8 rounded-full" style={{ backgroundColor: c }} />
      ))}
    </div>
  );
}

function AnimeMorphPath() {
  const pathRef = useRef<SVGPathElement>(null);

  useEffect(() => {
    if (!pathRef.current) return;
    const star = 'M50,5 L61,35 L95,35 L68,57 L79,91 L50,70 L21,91 L32,57 L5,35 L39,35 Z';
    const circle = 'M50,5 A45,45 0 1,1 49.99,5 Z';
    const diamond = 'M50,5 L90,50 L50,95 L10,50 Z';

    const tl = createTimeline({ loop: true });
    tl.add(pathRef.current, { d: circle, duration: 1000, easing: 'easeInOutQuad' })
      .add(pathRef.current, { d: diamond, duration: 1000, easing: 'easeInOutQuad' })
      .add(pathRef.current, { d: star, duration: 1000, easing: 'easeInOutQuad' });
    return () => { tl.pause(); };
  }, []);

  return (
    <div className="flex items-center justify-center h-full">
      <svg viewBox="0 0 100 100" width="120" height="120">
        <path
          ref={pathRef}
          d="M50,5 L61,35 L95,35 L68,57 L79,91 L50,70 L21,91 L32,57 L5,35 L39,35 Z"
          fill="#d4a843" stroke="#f0d080" strokeWidth="1"
        />
      </svg>
    </div>
  );
}

function AnimeParticleExplosion() {
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!ref.current) return;
    const container = ref.current;

    const fire = () => {
      // Create particles
      const particles: HTMLDivElement[] = [];
      for (let i = 0; i < 30; i++) {
        const p = document.createElement('div');
        p.style.cssText = 'position:absolute;width:6px;height:6px;border-radius:50%;left:50%;top:50%;pointer-events:none;';
        p.style.backgroundColor = ['#d4a843', '#f0d080', '#c44040', '#fff'][Math.floor(Math.random() * 4)];
        container.appendChild(p);
        particles.push(p);
      }

      animate(particles, {
        translateX: () => utils.random(-120, 120),
        translateY: () => utils.random(-120, 120),
        scale: [1, 0],
        opacity: [1, 0],
        duration: () => utils.random(600, 1200),
        easing: 'easeOutExpo',
        onComplete: () => { particles.forEach(p => p.remove()); },
      });
    };

    const interval = setInterval(fire, 2000);
    fire();
    return () => clearInterval(interval);
  }, []);

  return <div ref={ref} className="relative h-full overflow-hidden" />;
}

function AnimeWaveText() {
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!ref.current) return;
    const chars = ref.current.querySelectorAll('.wchar');
    const anim = animate(chars, {
      translateY: [-15, 0, -15],
      color: ['#d4a843', '#f0d080', '#d4a843'],
      duration: 1500,
      delay: stagger(80, { start: 0 }),
      loop: true,
      easing: 'easeInOutSine',
    });
    return () => { anim.pause(); };
  }, []);

  const text = 'CRPG Framework';
  return (
    <div ref={ref} className="flex items-center justify-center h-full">
      <div className="text-lg font-bold font-mono tracking-wider">
        {text.split('').map((ch, i) => (
          <span key={i} className="wchar inline-block text-gold">
            {ch === ' ' ? '\u00A0' : ch}
          </span>
        ))}
      </div>
    </div>
  );
}

function AnimeProgressRing() {
  const circleRef = useRef<SVGCircleElement>(null);
  const textRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!circleRef.current || !textRef.current) return;
    const circumference = 2 * Math.PI * 40;
    circleRef.current.style.strokeDasharray = String(circumference);
    circleRef.current.style.strokeDashoffset = String(circumference);

    const obj = { value: 0 };
    const anim = animate(obj, {
      value: 100,
      duration: 2500,
      easing: 'easeInOutQuart',
      loop: true,
      loopDelay: 800,
      onUpdate: () => {
        const v = Math.round(obj.value);
        if (circleRef.current) {
          circleRef.current.style.strokeDashoffset = String(circumference * (1 - v / 100));
        }
        if (textRef.current) {
          textRef.current.textContent = `${v}%`;
        }
      },
    });
    return () => { anim.pause(); };
  }, []);

  return (
    <div className="flex items-center justify-center h-full">
      <div className="relative">
        <svg width="100" height="100" className="rotate-[-90deg]">
          <circle cx="50" cy="50" r="40" fill="none" stroke="#333" strokeWidth="6" />
          <circle ref={circleRef} cx="50" cy="50" r="40" fill="none" stroke="#d4a843"
                  strokeWidth="6" strokeLinecap="round" />
        </svg>
        <div ref={textRef} className="absolute inset-0 flex items-center justify-center text-gold font-bold text-lg">
          0%
        </div>
      </div>
    </div>
  );
}

// ── Anime.js Advanced: Draggable + Layout ──────────────────

function AnimeDraggableFree() {
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!ref.current) return;
    const els = ref.current.querySelectorAll('.drag-item');
    const drags = Array.from(els).map(el => createDraggable(el, {
      container: ref.current!,
      releaseEase: 'outElastic(1, .3)',
      dragSpeed: 0.5,
    }));
    return () => { drags.forEach(d => d.disable()); };
  }, []);

  const colors = ['#d4a843', '#c44040', '#4a7acc', '#4caf4c', '#b8860b'];
  return (
    <div ref={ref} className="relative h-full w-full">
      {colors.map((c, i) => (
        <div key={i} className="drag-item absolute w-12 h-12 rounded-lg cursor-grab active:cursor-grabbing flex items-center justify-center text-xs font-bold text-black/60"
             style={{ backgroundColor: c, left: 20 + i * 50, top: 60 + (i % 2) * 50 }}>
          {i + 1}
        </div>
      ))}
      <div className="absolute bottom-2 left-0 right-0 text-center text-[10px] text-parchment/30">
        drag the blocks
      </div>
    </div>
  );
}

function AnimeDraggableSnap() {
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!ref.current) return;
    const el = ref.current.querySelector('.snap-box');
    if (!el) return;
    const drag = createDraggable(el, {
      container: ref.current,
      x: { snap: 60 },
      y: { snap: 60 },
      releaseEase: 'outExpo',
      dragSpeed: 0.5,
    });
    return () => { drag.disable(); };
  }, []);

  return (
    <div ref={ref} className="relative h-full w-full">
      <div className="absolute inset-0" style={{
        backgroundImage: 'radial-gradient(circle, #333 1px, transparent 1px)',
        backgroundSize: '60px 60px',
      }} />
      <div className="snap-box absolute w-14 h-14 bg-gold rounded cursor-grab active:cursor-grabbing"
           style={{ left: 60, top: 60 }} />
      <div className="absolute bottom-2 left-0 right-0 text-center text-[10px] text-parchment/30">
        snap to 60px grid
      </div>
    </div>
  );
}

function AnimeDraggableAxis() {
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!ref.current) return;
    const hBox = ref.current.querySelector('.h-drag');
    const vBox = ref.current.querySelector('.v-drag');
    const drags: ReturnType<typeof createDraggable>[] = [];
    if (hBox) {
      drags.push(createDraggable(hBox, {
        container: ref.current,
        y: false,
        releaseEase: 'outElastic(1, .4)',
        dragSpeed: 0.5,
      }));
    }
    if (vBox) {
      drags.push(createDraggable(vBox, {
        container: ref.current,
        x: false,
        releaseEase: 'outElastic(1, .4)',
        dragSpeed: 0.5,
      }));
    }
    return () => { drags.forEach(d => d.disable()); };
  }, []);

  return (
    <div ref={ref} className="relative h-full w-full flex items-center justify-center gap-8">
      <div className="relative w-48 h-2 bg-zinc-700 rounded-full">
        <div className="h-drag absolute -top-4 left-0 w-10 h-10 bg-sky-500 rounded-full cursor-grab active:cursor-grabbing" />
      </div>
      <div className="relative w-2 h-40 bg-zinc-700 rounded-full">
        <div className="v-drag absolute -left-4 top-0 w-10 h-10 bg-rose-500 rounded-full cursor-grab active:cursor-grabbing" />
      </div>
    </div>
  );
}

function AnimeDraggableThrow() {
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!ref.current) return;
    const ball = ref.current.querySelector('.throw-ball');
    if (!ball) return;
    const drag = createDraggable(ball, {
      container: ref.current,
      containerFriction: 0.5,
      releaseEase: 'outExpo',
      dragSpeed: 0.5,
    });
    return () => { drag.disable(); };
  }, []);

  return (
    <div ref={ref} className="relative h-full w-full border border-zinc-700 rounded overflow-hidden">
      <div className="throw-ball absolute w-10 h-10 bg-amber-400 rounded-full cursor-grab active:cursor-grabbing shadow-lg"
           style={{ left: '50%', top: '50%', transform: 'translate(-50%,-50%)' }} />
      <div className="absolute bottom-2 left-0 right-0 text-center text-[10px] text-parchment/30">
        throw with momentum
      </div>
    </div>
  );
}

function AnimeLayoutDomShuffle() {
  const wrapRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!wrapRef.current) return;
    const container = wrapRef.current.querySelector('.shuffle-container') as HTMLElement;
    if (!container) return;

    container.innerHTML = '';

    const items = ['Sword', 'Shield', 'Potion', 'Scroll', 'Ring', 'Helm'];
    const colors = ['#d4a843', '#c44040', '#4a7acc', '#4caf4c', '#b8860b', '#9333ea'];
    items.forEach((name, i) => {
      const el = document.createElement('div');
      el.className = 'dom-item';
      el.style.cssText = `background:${colors[i]};width:72px;height:32px;line-height:32px;border-radius:6px;font-size:12px;font-weight:bold;color:rgba(0,0,0,0.7);text-align:center;`;
      el.textContent = name;
      container.appendChild(el);
    });

    const btn = wrapRef.current.querySelector('.shuffle-btn');
    const onClick = () => {
      const els = [...container.querySelectorAll('.dom-item')] as HTMLElement[];
      // Clear in-flight transforms so offsetLeft/Top are accurate
      els.forEach(el => { el.style.transform = ''; });
      void container.offsetHeight;
      // Record positions relative to container
      const oldPos = new Map(els.map(el => [el, { x: el.offsetLeft, y: el.offsetTop }]));

      // Shuffle DOM order
      utils.shuffle(els).forEach(el => container.appendChild(el));
      void container.offsetHeight;

      // FLIP: animate from old offset to new offset
      els.forEach(el => {
        const old = oldPos.get(el)!;
        const dx = old.x - el.offsetLeft;
        const dy = old.y - el.offsetTop;
        if (Math.abs(dx) < 1 && Math.abs(dy) < 1) return;
        animate(el, {
          translateX: [dx, 0],
          translateY: [dy, 0],
          duration: 600,
          easing: 'easeOutExpo',
        });
      });
    };
    btn?.addEventListener('click', onClick);
    return () => { btn?.removeEventListener('click', onClick); };
  }, []);

  return (
    <div ref={wrapRef} className="h-full flex flex-col p-3 gap-2">
      <button className="shuffle-btn self-center px-3 py-1 text-[10px] bg-zinc-700 hover:bg-zinc-600 rounded text-zinc-300 transition-colors">
        Shuffle DOM Order
      </button>
      <div className="shuffle-container flex-1 flex flex-wrap gap-2 content-start overflow-hidden" />
    </div>
  );
}

function AnimeLayoutDisplay() {
  const wrapRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!wrapRef.current) return;
    const container = wrapRef.current.querySelector('.display-container') as HTMLElement;
    const btn = wrapRef.current.querySelector('.display-btn') as HTMLElement;
    if (!container || !btn) return;

    container.innerHTML = '';

    const stats = ['STR', 'DEX', 'INT', 'WIS', 'CHA', 'CON'];
    stats.forEach(stat => {
      const el = document.createElement('div');
      el.className = 'display-item';
      el.style.cssText = 'background:#27272a;border:1px solid rgba(212,168,67,0.2);border-radius:6px;width:64px;height:32px;line-height:32px;font-size:12px;color:#d4a843;text-align:center;flex-shrink:0;';
      el.textContent = stat;
      container.appendChild(el);
    });

    const modes = ['flex-row', 'grid-2col', 'flex-col', 'grid-3col', 'flex-row-reverse'];
    const modeCSS: Record<string, string> = {
      'flex-row':         'display:flex;flex-direction:row;flex-wrap:wrap;gap:8px;',
      'grid-2col':        'display:grid;grid-template-columns:1fr 1fr;gap:8px;',
      'flex-col':         'display:flex;flex-direction:column;gap:8px;',
      'grid-3col':        'display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;',
      'flex-row-reverse': 'display:flex;flex-direction:row-reverse;flex-wrap:wrap;gap:8px;',
    };

    let idx = 0;
    container.style.cssText = modeCSS[modes[idx]];
    btn.textContent = modes[idx];

    btn.addEventListener('click', () => {
      const els = [...container.querySelectorAll('.display-item')] as HTMLElement[];
      els.forEach(el => { el.style.transform = ''; });
      void container.offsetHeight;
      const oldPos = els.map(el => ({ x: el.offsetLeft, y: el.offsetTop }));

      idx = (idx + 1) % modes.length;
      container.style.cssText = modeCSS[modes[idx]];
      btn.textContent = modes[idx];

      void container.offsetHeight;
      els.forEach((el, i) => {
        const dx = oldPos[i].x - el.offsetLeft;
        const dy = oldPos[i].y - el.offsetTop;
        if (Math.abs(dx) < 1 && Math.abs(dy) < 1) return;
        animate(el, {
          translateX: [dx, 0],
          translateY: [dy, 0],
          duration: 500,
          easing: 'easeOutExpo',
        });
      });
    });

    return () => { container.innerHTML = ''; };
  }, []);

  return (
    <div ref={wrapRef} className="h-full flex flex-col p-3 gap-2">
      <button className="display-btn self-center px-3 py-1 text-[10px] bg-zinc-700 hover:bg-zinc-600 rounded text-zinc-300 transition-colors font-mono">
        loading...
      </button>
      <div className="display-container flex-1 overflow-auto" />
    </div>
  );
}

// ═══════════════════════════════════════════════════════════
// Lottie Tests
// ═══════════════════════════════════════════════════════════

/** Load a lottie animation from URL into a container */
function LottieFromURL({ url }: { url: string }) {
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!ref.current) return;
    const anim = lottie.loadAnimation({
      container: ref.current,
      renderer: 'svg',
      loop: true,
      autoplay: true,
      path: url,
    });
    return () => { anim.destroy(); };
  }, [url]);

  return <div ref={ref} className="w-full h-full flex items-center justify-center" />;
}

// Verified working LottieFiles CDN URLs
const LOTTIE_TESTS: TestCell[] = [
  { label: 'Lottie: Checkmark', render: () => <LottieFromURL url="https://assets2.lottiefiles.com/packages/lf20_usmfx6bp.json" /> },
  { label: 'Lottie: Success',   render: () => <LottieFromURL url="https://assets10.lottiefiles.com/packages/lf20_jcikwtux.json" /> },
  { label: 'Lottie: Confetti',  render: () => <LottieFromURL url="https://assets5.lottiefiles.com/packages/lf20_obhph3sh.json" /> },
  { label: 'Lottie: Trophy',    render: () => <LottieFromURL url="https://assets3.lottiefiles.com/packages/lf20_touohxv0.json" /> },
];

// ── Test Arrays ────────────────────────────────────────────

const ANIME_TESTS: TestCell[] = [
  // Core
  { label: 'Stagger Grid (5x5)', render: () => <AnimeStaggerGrid /> },
  { label: 'Text Reveal', render: () => <AnimeTextReveal /> },
  { label: 'Timeline Sequence', render: () => <AnimeTimeline /> },
  { label: 'Elastic Bounce', render: () => <AnimeElasticBounce /> },
  { label: 'SVG Morph Path', render: () => <AnimeMorphPath /> },
  { label: 'Particle Explosion', render: () => <AnimeParticleExplosion /> },
  { label: 'Wave Text', render: () => <AnimeWaveText /> },
  { label: 'Progress Ring', render: () => <AnimeProgressRing /> },
  // Draggable
  { label: 'Draggable: Free', render: () => <AnimeDraggableFree /> },
  { label: 'Draggable: Snap Grid', render: () => <AnimeDraggableSnap /> },
  { label: 'Draggable: Axis Lock', render: () => <AnimeDraggableAxis /> },
  { label: 'Draggable: Throw', render: () => <AnimeDraggableThrow /> },
  // Layout (createLayout API)
  { label: 'Layout: DOM Shuffle', render: () => <AnimeLayoutDomShuffle /> },
  { label: 'Layout: Display Cycle', render: () => <AnimeLayoutDisplay /> },
];

// ── Test Arrays ────────────────────────────────────────────

const FRAMER_TESTS: TestCell[] = [
  { label: 'Fade In', render: () => <FramerFadeIn /> },
  { label: 'Spring Physics', render: () => <FramerSpring /> },
  { label: 'Hover + Tap', render: () => <FramerGesture /> },
  { label: 'Layout Animation', render: () => <FramerLayoutAnim /> },
  { label: 'Stagger List', render: () => <FramerStagger /> },
  { label: 'AnimatePresence', render: () => <FramerPresence /> },
];

const SPRING_TESTS: TestCell[] = [
  { label: 'Animated Number', render: () => <SpringNumber /> },
  { label: 'Trail Animation', render: () => <SpringTrail /> },
  { label: 'Progress Bar', render: () => <SpringProgressBar /> },
];

const ICON_TESTS: TestCell[] = [
  { label: 'Lucide Icon Grid (SVG)', size: 'large', render: () => <LucideIconGrid /> },
  { label: 'Animated Icon Cycle', size: 'large', render: () => <LucideAnimatedIcons /> },
];

export function TestGallery() {
  const handleClick = useCallback((label: string) => {
    sendToUE('test_cell_click', { label });
  }, []);

  return (
    <div className="absolute inset-0 bg-[#0a0806] overflow-y-auto p-6">
      {/* CSS Tests */}
      <h1 className="text-xl font-bold text-gold mb-4 tracking-wider">
        CSS Features ({CSS_TESTS.length})
      </h1>
      <div className="grid grid-cols-4 gap-3 mb-10">
        {CSS_TESTS.map((test) => (
          <div key={test.label}
            className="flex flex-col border border-gold/20 rounded overflow-hidden cursor-pointer hover:border-gold/60 transition-colors"
            onClick={() => handleClick(test.label)}
          >
            <div className="h-72 bg-black/50 relative overflow-hidden">
              {test.render()}
            </div>
            <div className="px-3 py-1.5 text-xs text-parchment/60 text-center truncate bg-black/80">
              {test.label}
            </div>
          </div>
        ))}
      </div>

      {/* D3 Tests */}
      <h1 className="text-xl font-bold text-gold mb-4 tracking-wider">
        D3 Data Visualization ({D3_TESTS.length})
      </h1>
      <div className="grid grid-cols-4 gap-4 mb-10">
        {D3_TESTS.map((test) => (
          <div key={test.label}
            className="flex flex-col border border-gold/20 rounded overflow-hidden cursor-pointer hover:border-gold/60 transition-colors"
            onClick={() => handleClick(test.label)}
          >
            <div className="h-96 bg-black/50 relative overflow-hidden flex items-center justify-center">
              {test.render()}
            </div>
            <div className="px-3 py-2 text-sm text-parchment/60 text-center truncate bg-black/80">
              {test.label}
            </div>
          </div>
        ))}
      </div>

      {/* D3 Interactive — full width cards */}
      <h1 className="text-xl font-bold text-gold mb-4 tracking-wider">
        D3 Interactive ({D3_INTERACTIVE_TESTS.length})
      </h1>
      <div className="grid grid-cols-1 gap-4 mb-10">
        {D3_INTERACTIVE_TESTS.map((test) => (
          <div key={test.label}
            className="flex flex-col border border-gold/20 rounded overflow-hidden"
          >
            <div className="h-[1500px] bg-black/50 relative overflow-hidden flex items-center justify-center p-4">
              {test.render()}
            </div>
            <div className="px-3 py-2 text-sm text-parchment/60 text-center bg-black/80">
              {test.label}
            </div>
          </div>
        ))}
      </div>

      {/* WebGL / Canvas Tests */}
      <h1 className="text-xl font-bold text-gold mb-4 tracking-wider">
        K-Line / Candlestick ({KLINE_TESTS.length})
      </h1>
      <div className="grid grid-cols-1 gap-4 mb-10">
        {KLINE_TESTS.map((test) => (
          <div key={test.label}
            className="flex flex-col border border-gold/20 rounded overflow-hidden"
          >
            <div className="h-[800px] bg-[#0a0806] relative overflow-hidden">
              {test.render()}
            </div>
            <div className="px-3 py-2 text-sm text-parchment/60 text-center bg-black/80">
              {test.label}
            </div>
          </div>
        ))}
      </div>

      {/* WebGL / Canvas Tests */}
      <h1 className="text-xl font-bold text-gold mb-4 tracking-wider">
        Canvas2D &amp; Three.js WebGL ({WEBGL_TESTS.length})
      </h1>
      <div className="grid grid-cols-1 gap-4 mb-10">
        {WEBGL_TESTS.map((test) => (
          <div key={test.label}
            className="flex flex-col border border-gold/20 rounded overflow-hidden"
          >
            <div className="h-[600px] bg-black relative overflow-hidden">
              {test.render()}
            </div>
            <div className="px-3 py-2 text-sm text-parchment/60 text-center bg-black/80">
              {test.label}
            </div>
          </div>
        ))}
      </div>

      {/* Framer Motion Tests */}
      <h1 className="text-xl font-bold text-gold mb-4 tracking-wider">
        Framer Motion ({FRAMER_TESTS.length})
      </h1>
      <div className="grid grid-cols-3 gap-4 mb-10">
        {FRAMER_TESTS.map((test) => (
          <div key={test.label}
            className="flex flex-col border border-gold/20 rounded overflow-hidden cursor-pointer hover:border-gold/60 transition-colors"
            onClick={() => handleClick(test.label)}
          >
            <div className="h-80 bg-black/50 relative overflow-hidden">
              {test.render()}
            </div>
            <div className="px-3 py-2 text-sm text-parchment/60 text-center truncate bg-black/80">
              {test.label}
            </div>
          </div>
        ))}
      </div>

      {/* React Spring Tests */}
      <h1 className="text-xl font-bold text-gold mb-4 tracking-wider">
        React Spring ({SPRING_TESTS.length})
      </h1>
      <div className="grid grid-cols-3 gap-4 mb-10">
        {SPRING_TESTS.map((test) => (
          <div key={test.label}
            className="flex flex-col border border-gold/20 rounded overflow-hidden cursor-pointer hover:border-gold/60 transition-colors"
            onClick={() => handleClick(test.label)}
          >
            <div className="h-72 bg-black/50 relative overflow-hidden">
              {test.render()}
            </div>
            <div className="px-3 py-2 text-sm text-parchment/60 text-center truncate bg-black/80">
              {test.label}
            </div>
          </div>
        ))}
      </div>

      {/* Icon Tests */}
      <h1 className="text-xl font-bold text-gold mb-4 tracking-wider">
        Lucide Icons (SVG) ({ICON_TESTS.length})
      </h1>
      <div className="grid grid-cols-2 gap-4 mb-10">
        {ICON_TESTS.map((test) => (
          <div key={test.label}
            className="flex flex-col border border-gold/20 rounded overflow-hidden cursor-pointer hover:border-gold/60 transition-colors"
            onClick={() => handleClick(test.label)}
          >
            <div className="h-72 bg-black/50 relative overflow-hidden flex items-center justify-center">
              {test.render()}
            </div>
            <div className="px-3 py-2 text-sm text-parchment/60 text-center truncate bg-black/80">
              {test.label}
            </div>
          </div>
        ))}
      </div>

      {/* Anime.js Tests */}
      <h1 className="text-xl font-bold text-gold mb-4 tracking-wider">
        Anime.js ({ANIME_TESTS.length})
      </h1>
      <div className="grid grid-cols-4 gap-3 mb-10">
        {ANIME_TESTS.map((test) => (
          <div key={test.label}
            className="flex flex-col border border-gold/20 rounded overflow-hidden cursor-pointer hover:border-gold/60 transition-colors"
            onClick={() => handleClick(test.label)}
          >
            <div className="h-72 bg-black/50 relative overflow-hidden">
              {test.render()}
            </div>
            <div className="px-3 py-1.5 text-xs text-parchment/60 text-center truncate bg-black/80">
              {test.label}
            </div>
          </div>
        ))}
      </div>

      {/* Lottie Tests */}
      <h1 className="text-xl font-bold text-gold mb-4 tracking-wider">
        Lottie ({LOTTIE_TESTS.length})
      </h1>
      <div className="grid grid-cols-4 gap-3 mb-10">
        {LOTTIE_TESTS.map((test) => (
          <div key={test.label}
            className="flex flex-col border border-gold/20 rounded overflow-hidden cursor-pointer hover:border-gold/60 transition-colors"
            onClick={() => handleClick(test.label)}
          >
            <div className="h-72 bg-black/50 relative overflow-hidden">
              {test.render()}
            </div>
            <div className="px-3 py-1.5 text-xs text-parchment/60 text-center truncate bg-black/80">
              {test.label}
            </div>
          </div>
        ))}
      </div>

      {/* Bridge Test */}
      <div className="text-xs text-parchment/20 font-mono pb-8">
        Click any card → sends sendToUE('test_cell_click') — check UE Output Log
      </div>
    </div>
  );
}
