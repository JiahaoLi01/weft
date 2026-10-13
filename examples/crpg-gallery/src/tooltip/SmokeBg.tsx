import { useState } from 'react';

// The CRPG UI's smoke background as the crpg-smoke UI shader effect (examples/crpg-gallery/effects/smoke.hlsl) in place
// of tests/fixtures/crpg-gallery/UI/src/components/SmokeBg.tsx, whose canvas the original computes on the CPU about 15
// times a second: the same exports and props, the same box, and the canvas's parameters as the effect's custom
// properties. The effect computes every frame what the original's canvas shows at that time; its clock starts at the
// mount, as the original's does.

// ===== Params (as the original's) =====

export interface SmokeParams {
  cloudScale: number;
  noiseScale: number;
  cloudSpeedX: number;
  cloudSpeedY: number;
  noiseSpeedX: number;
  noiseSpeedY: number;
  gamma: number;
  noiseStrength: number;
  fadePower: number;
}

export const DEFAULT_SMOKE_PARAMS: SmokeParams = {
  cloudScale: 7.2,
  noiseScale: 2.8,
  cloudSpeedX: 1,
  cloudSpeedY: 1,
  noiseSpeedX: -6,
  noiseSpeedY: -10,
  gamma: 0.1,
  noiseStrength: 0.3,
  fadePower: 2.0,
};

const RARITY_RGB: Record<string, [number, number, number]> = {
  common:    [156, 163, 175],
  rare:      [96, 165, 250],
  epic:      [192, 132, 252],
  legendary: [245, 158, 11],
  special:   [212, 184, 130],
};

export type SmokeFade = 'top' | 'right';

// The original's canvas size.
const CW = 48;
const CH = 64;

// ===== Component =====

export function SmokeBg({
  rarity,
  params,
  fade = 'top',
  rotate90 = false,
  resolution,
  className,
}: {
  rarity: string;
  params?: SmokeParams;
  fade?: SmokeFade;
  rotate90?: boolean;
  resolution?: { w: number; h: number };
  className?: string;
}) {
  const p = params ?? DEFAULT_SMOKE_PARAMS;
  // The effect's time, in seconds, at the mount: the original's clock starts there.
  const [start] = useState(() => performance.now() / 1000);
  const rgb = RARITY_RGB[rarity] ?? RARITY_RGB.common;
  const style: Record<string, string> = {
    backgroundImage: '-weft-effect(crpg-smoke)',
    '--smoke-scales': `${p.cloudScale} ${p.noiseScale} ${p.gamma} ${p.noiseStrength}`,
    '--smoke-speeds': `${p.cloudSpeedX} ${p.cloudSpeedY} ${p.noiseSpeedX} ${p.noiseSpeedY}`,
    '--smoke-shape': `${p.fadePower} ${fade === 'right' ? 1 : 0} ${rotate90 ? 1 : 0}`,
    '--smoke-canvas': `${resolution?.w ?? CW} ${resolution?.h ?? CH}`,
    '--smoke-color': `rgb(${rgb[0]}, ${rgb[1]}, ${rgb[2]})`,
    '--smoke-start': String(start),
  };
  return <div className={className ?? 'absolute inset-0 w-full h-full rounded-lg pointer-events-none'} style={style} />;
}
