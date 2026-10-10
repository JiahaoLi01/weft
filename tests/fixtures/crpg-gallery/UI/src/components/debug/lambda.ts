/**
 * Gamma PDF 与 lambda(t) 计算——Inspector 面板里画 RumorDefinition 的"理论曲线"用。
 *
 * 实现照搬 mini-app/rumor-simulator 的 simulation.js（log 域 Lanczos lgamma）——
 * 同一份数学，前后端结果一致，便于对照调试。
 */

const LANCZOS_C = [
  676.5203681218851,
  -1259.1392167224028,
  771.3234287776531,
  -176.6150291621406,
  12.507343278686905,
  -0.13857109526572012,
  9.984369578019572e-6,
  1.5056327351493116e-7,
];

function lgamma(z: number): number {
  if (z < 0.5) {
    return Math.log(Math.PI) - Math.log(Math.sin(Math.PI * z)) - lgamma(1 - z);
  }
  let x = 0.9999999999998099;
  const shifted = z - 1;
  for (let i = 0; i < LANCZOS_C.length; i += 1) {
    x += LANCZOS_C[i] / (shifted + i + 1);
  }
  const t = shifted + LANCZOS_C.length - 0.5;
  return 0.5 * Math.log(2 * Math.PI) + (shifted + 0.5) * Math.log(t) - t + Math.log(x);
}

export function gammaPdf(x: number, shape: number, scale: number): number {
  if (x <= 0 || shape <= 0 || scale <= 0) return 0;
  const log = (shape - 1) * Math.log(x) - x / scale - lgamma(shape) - shape * Math.log(scale);
  return Math.exp(log);
}

export interface LambdaArgs {
  isBaseline: boolean;
  amplitude: number;
  birthTick: number;
  shape: number;
  scale: number;
}

/** 单条 rumor 在 tick t 的注入项 lambda(t)。 */
export function lambdaAt(t: number, args: LambdaArgs): number {
  if (args.isBaseline) return args.amplitude;
  return args.amplitude * gammaPdf(t - args.birthTick, args.shape, args.scale);
}

/** 理论峰值 tick（k > 1 时存在）。 */
export function peakTick(args: LambdaArgs): number | null {
  if (args.isBaseline) return null;
  if (args.shape <= 1) return args.birthTick;
  return args.birthTick + (args.shape - 1) * args.scale;
}

/** 变体触发概率 p = 1 - exp(-mu * I * dt)。 */
export function variantProbability(intensity: number, variantRate: number, dt = 1): number {
  return 1 - Math.exp(-variantRate * Math.max(0, intensity) * dt);
}
