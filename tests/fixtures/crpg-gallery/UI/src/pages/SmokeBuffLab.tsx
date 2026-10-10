import { useCallback, useState } from 'react';
import { DEFAULT_SMOKE_PARAMS, SmokeBg, type SmokeFade, type SmokeParams } from '@/components/SmokeBg';

/**
 * SmokeBuffLab —— special buff 烟雾的**调参实验室**(dev 页,page id: 'smoke-buff-lab')。
 *
 * 左侧:1:1 复刻状态面板 special chip 的两个样张(单行 / 三行描述,验证不同高度下的
 * 拉伸观感);右侧:全参数面板(SmokeParams + 淡出轴 / rotate90 / 画布分辨率)。
 * 底部输出当前配置 JSON —— 调好后整段复制发给 Claude,回填到
 * BattleUnitStatusPanel 的 SpecialBuffSmoke。
 */

// 与 TONE_CHIP_STYLES.special 保持一致的卡面基底(demo 独立复刻,不 import 面板内部)。
const CHIP_BASE = {
  border: '1px solid rgba(199, 166, 103, 0.58)',
  background: 'rgba(39, 31, 20, 0.76)',
  boxShadow: [
    'inset 0 1px 0 rgba(239, 220, 174, 0.15)',
    'inset 0 -1px 0 rgba(0, 0, 0, 0.72)',
    'inset 1px 0 0 rgba(239, 220, 174, 0.07)',
    'inset -1px 0 0 rgba(0, 0, 0, 0.45)',
    '0 1px 0 rgba(0, 0, 0, 0.28)',
  ].join(', '),
} as const;

interface LabConfig {
  params: SmokeParams;
  fade: SmokeFade;
  rotate90: boolean;
  resolution: { w: number; h: number };
}

function MockChip({ cfg, lines }: { cfg: LabConfig; lines: 1 | 3 }) {
  return (
    <div
      className="relative overflow-hidden px-2 py-1.5 rounded-[3px] w-[296px]"
      style={CHIP_BASE}
    >
      <SmokeBg
        rarity="special"
        params={cfg.params}
        fade={cfg.fade}
        rotate90={cfg.rotate90}
        resolution={cfg.resolution}
        className="absolute inset-0 w-full h-full pointer-events-none"
      />
      <div className="relative z-10 flex items-center gap-1.5">
        <span
          className="size-1.5 rounded-full shrink-0"
          style={{
            background: '#c4ad78',
            boxShadow:
              '-0.5px -0.5px 0 rgba(239, 220, 174, 0.42), 0.5px 0.5px 0 rgba(0, 0, 0, 0.8), inset 0 0 0 1px rgba(82, 61, 29, 0.75)',
          }}
          aria-hidden
        />
        <span className="text-[11px] font-semibold text-parchment truncate font-[family-name:var(--font-display)]">
          风险流浪者
        </span>
        <span className="ml-auto shrink-0 text-[9px] tabular-nums text-parchment-dark/70 font-[family-name:var(--font-subtitle)]">
          永久
        </span>
      </div>
      <div className="text-[10px] leading-snug text-parchment-dark/85 mt-0.5 pl-3 whitespace-pre-line break-words font-[family-name:var(--font-subtitle)]">
        {lines === 1
          ? '这个生物承受或造成的伤害在掷骰时均取最大值。'
          : '这个生物承受或造成的伤害在掷骰时均取最大值。命运的天平不再摇摆,每一次掷骰都落在刻度的尽头。旁观者说这是祝福,保险精算师说这是灾难。'}
      </div>
    </div>
  );
}

export function SmokeBuffLab() {
  const [params, setParams] = useState<SmokeParams>({ ...DEFAULT_SMOKE_PARAMS });
  const [fade, setFade] = useState<SmokeFade>('right');
  const [rotate90, setRotate90] = useState(true);
  const [resW, setResW] = useState(96);
  const [resH, setResH] = useState(24);

  const cfg: LabConfig = { params, fade, rotate90, resolution: { w: resW, h: resH } };

  const slider = useCallback(
    (key: keyof SmokeParams, min: number, max: number, step: number) => (
      <label key={key} className="flex items-center gap-2">
        <span className="text-[10px] text-zinc-400 w-24 shrink-0">{key}</span>
        <input
          type="range" min={min} max={max} step={step} value={params[key]}
          onChange={(e) => setParams((prev) => ({ ...prev, [key]: parseFloat(e.target.value) }))}
          className="w-28 h-1 accent-zinc-400 cursor-pointer"
        />
        <span className="text-[10px] text-zinc-300 w-12 text-right font-mono">{params[key].toFixed(2)}</span>
      </label>
    ),
    [params],
  );

  const numSlider = (label: string, value: number, set: (v: number) => void, min: number, max: number, step: number) => (
    <label className="flex items-center gap-2">
      <span className="text-[10px] text-zinc-400 w-24 shrink-0">{label}</span>
      <input
        type="range" min={min} max={max} step={step} value={value}
        onChange={(e) => set(parseInt(e.target.value, 10))}
        className="w-28 h-1 accent-zinc-400 cursor-pointer"
      />
      <span className="text-[10px] text-zinc-300 w-12 text-right font-mono">{value}</span>
    </label>
  );

  return (
    <div className="w-full h-full flex items-start justify-center gap-8 p-10 bg-[#111]">
      {/* 样张列 */}
      <div className="flex flex-col gap-4 pt-6">
        <div className="text-xs text-zinc-500">special buff chip 样张(1:1 尺寸)</div>
        <MockChip cfg={cfg} lines={1} />
        <MockChip cfg={cfg} lines={3} />

        {/* 当前配置输出 —— 调好后整段复制发给 Claude */}
        <div className="mt-4 max-w-[296px]">
          <div className="text-[10px] text-zinc-500 mb-1">当前配置(调好后把这段发给 Claude):</div>
          <pre className="text-[10px] leading-snug text-zinc-300 bg-zinc-900 border border-zinc-700 rounded p-2 select-text whitespace-pre-wrap">
            {JSON.stringify(cfg, null, 2)}
          </pre>
        </div>
      </div>

      {/* 参数面板 */}
      <div className="bg-zinc-800 border border-zinc-700 rounded-lg px-3 py-2 select-none mt-6">
        <div className="text-[10px] text-zinc-400 font-bold mb-2">SPECIAL BUFF SMOKE PARAMS</div>
        <div className="flex flex-col gap-1.5">
          {slider('cloudScale', 0.5, 20, 0.1)}
          {slider('noiseScale', 0.5, 20, 0.1)}
          {slider('cloudSpeedX', -30, 30, 0.5)}
          {slider('cloudSpeedY', -30, 30, 0.5)}
          {slider('noiseSpeedX', -30, 30, 0.5)}
          {slider('noiseSpeedY', -30, 30, 0.5)}
          <div className="h-px bg-zinc-700 my-1" />
          {slider('gamma', 0.05, 3, 0.05)}
          {slider('noiseStrength', 0, 1, 0.05)}
          {slider('fadePower', 0.1, 5, 0.1)}
          <div className="h-px bg-zinc-700 my-1" />
          {numSlider('canvas W', resW, setResW, 16, 192, 8)}
          {numSlider('canvas H', resH, setResH, 8, 96, 4)}
          <label className="flex items-center gap-2 text-[10px] text-zinc-400">
            <span className="w-24 shrink-0">rotate90</span>
            <input type="checkbox" checked={rotate90} onChange={(e) => setRotate90(e.target.checked)} />
            <span className="text-zinc-500">纹理躺倒(宽扁容器)</span>
          </label>
          <label className="flex items-center gap-2 text-[10px] text-zinc-400">
            <span className="w-24 shrink-0">fade</span>
            <select
              value={fade}
              onChange={(e) => setFade(e.target.value as SmokeFade)}
              className="bg-zinc-900 border border-zinc-700 rounded text-zinc-300 text-[10px] px-1 py-0.5"
            >
              <option value="right">right(右浓左淡)</option>
              <option value="top">top(上浓下淡)</option>
            </select>
          </label>
          <button
            className="mt-1 text-[10px] text-zinc-500 hover:text-zinc-300 text-left"
            onClick={() => {
              setParams({ ...DEFAULT_SMOKE_PARAMS });
              setFade('right');
              setRotate90(true);
              setResW(96);
              setResH(24);
            }}
          >
            Reset defaults
          </button>
        </div>
      </div>
    </div>
  );
}
