import { useRef, useEffect, useState, useCallback } from 'react';

// ===== Params =====

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
  // 战斗面板 special buff(Meta.Special)—— 古金/黄铜,与油画面板同色系,不走霓虹。
  special:   [212, 184, 130],
};

/** 淡出轴:'top' = 上浓下淡(竖版卡片默认);'right' = 右浓左淡(宽扁 buff chip,烟从右缘往左飘散)。 */
export type SmokeFade = 'top' | 'right';

// ── Tunables ────────────────────────────────────────────────
//
// The smoke is an ambient effect — the eye does not need 60fps. Capping
// the tick rate is the single biggest CPU win for this component (see
// commit history for the canvas2D / CSS / canvas2D-optimized journey).
//
// CW × CH = 3072 px (vs the original 4800 px) is a free ~36% reduction;
// at the visible card size the canvas is upscaled by CSS so the slight
// resolution drop is invisible in the soft, blurry smoke pattern.
const CW = 48;
const CH = 64;
const TICK_MS = 66; // ≈ 15 fps

// ===== Image loader =====

interface TexData {
  data: Uint8Array;
  w: number;
  h: number;
}
type TexPair = { cloud: TexData; noise: TexData };

let textures: TexPair | null = null;
let texPromise: Promise<TexPair> | null = null;

function loadTextures(): Promise<TexPair> {
  if (textures) return Promise.resolve(textures);
  if (texPromise) return texPromise;

  texPromise = new Promise(resolve => {
    const cloudImg = document.createElement('img');
    const noiseImg = document.createElement('img');
    cloudImg.style.display = 'none';
    noiseImg.style.display = 'none';
    document.body.appendChild(cloudImg);
    document.body.appendChild(noiseImg);

    let n = 0;
    const done = () => {
      if (++n < 2) return;
      textures = { cloud: extractLum(cloudImg), noise: extractLum(noiseImg) };
      cloudImg.remove();
      noiseImg.remove();
      resolve(textures);
    };
    cloudImg.onload = done; noiseImg.onload = done;
    cloudImg.onerror = () => console.error('[SmokeBg] load failed: cloud.png');
    noiseImg.onerror = () => console.error('[SmokeBg] load failed: tiling-noise.png');
    cloudImg.src = './cloud.png';
    noiseImg.src = './tiling-noise.png';
  });
  return texPromise;
}

function extractLum(img: HTMLImageElement): TexData {
  const c = document.createElement('canvas');
  c.width = img.width; c.height = img.height;
  const ctx = c.getContext('2d')!;
  ctx.drawImage(img, 0, 0);
  const id = ctx.getImageData(0, 0, c.width, c.height);
  const lum = new Uint8Array(c.width * c.height);
  for (let i = 0; i < lum.length; i++) lum[i] = id.data[i * 4];
  return { data: lum, w: c.width, h: c.height };
}

// ===== Animation tick =====

const SMOKE_KEY = '__smoke';

interface SmokeRefs {
  paramsRef: React.RefObject<SmokeParams>;
  rarityRef: React.RefObject<string>;
  texRef: React.RefObject<TexPair | null>;
  visibleRef: React.RefObject<boolean>;
  fadeRef: React.RefObject<SmokeFade>;
  rotRef: React.RefObject<boolean>;
}

function startLoop(canvas: HTMLCanvasElement, refs: SmokeRefs) {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  const prev = !!(canvas as any)[SMOKE_KEY];
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  (canvas as any)[SMOKE_KEY] = refs;
  if (prev) return;

  const ctx = canvas.getContext('2d')!;
  const w = canvas.width, h = canvas.height;
  const imageData = ctx.createImageData(w, h);
  const buf = imageData.data;

  const gammaLUT = new Uint8Array(256);
  let cachedGamma = -1;
  // 淡出 LUT 按轴取长:'top' 按行(h),'right' 按列(w)。轴变化时整表重建。
  const fadeLUT = new Float32Array(Math.max(w, h));
  let cachedFade = -1;
  let cachedAxis: SmokeFade | null = null;

  function rebuildLUTs(gamma: number, fadePower: number, fade: SmokeFade) {
    if (gamma !== cachedGamma) {
      for (let i = 0; i < 256; i++) gammaLUT[i] = (Math.pow(i / 255, gamma) * 255) | 0;
      cachedGamma = gamma;
    }
    if (fadePower !== cachedFade || fade !== cachedAxis) {
      if (fade === 'right') {
        // 右浓左淡:x=0 → 0,x=w → 1(烟从右缘生成,向左飘散消隐)
        for (let x = 0; x < w; x++) fadeLUT[x] = Math.pow(x / w, fadePower);
      } else {
        for (let y = 0; y < h; y++) fadeLUT[y] = Math.pow(1 - y / h, fadePower);
      }
      cachedFade = fadePower;
      cachedAxis = fade;
    }
  }

  let timer: ReturnType<typeof setTimeout> | null = null;
  let startedAt = performance.now();

  function tick() {
    if (!canvas.isConnected) {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      delete (canvas as any)[SMOKE_KEY];
      if (timer) clearTimeout(timer);
      return;
    }
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const r = (canvas as any)[SMOKE_KEY] as SmokeRefs | undefined;
    if (!r) return;

    // Skip the per-pixel work entirely while off-screen — the IntersectionObserver
    // sets visibleRef.current. We still keep the timer alive so we resume on view.
    if (!r.visibleRef.current) {
      timer = setTimeout(tick, TICK_MS * 4);
      return;
    }

    const tex = r.texRef.current;
    if (!tex) { timer = setTimeout(tick, TICK_MS); return; }

    const p = r.paramsRef.current;
    const rgb = RARITY_RGB[r.rarityRef.current] ?? RARITY_RGB.common;
    const fadeAxis = r.fadeRef.current;
    const t = (performance.now() - startedAt) * 0.001;

    rebuildLUTs(p.gamma, p.fadePower, fadeAxis);

    const cPxPerU = tex.cloud.w / p.cloudScale;
    const nPxPerU = tex.noise.w / p.noiseScale;
    const cox = t * p.cloudSpeedX * (cPxPerU / w);
    const coy = t * p.cloudSpeedY * (cPxPerU / h);
    const nox = t * p.noiseSpeedX * (nPxPerU / w);
    const noy = t * p.noiseSpeedY * (nPxPerU / h);

    const useNoise = p.noiseStrength > 0.01;
    const nFloor = 1.0 - p.noiseStrength;

    const r0 = rgb[0], g0 = rgb[1], b0 = rgb[2];
    const cData = tex.cloud.data, cW = tex.cloud.w, cH = tex.cloud.h;
    const nData = tex.noise.data, nW = tex.noise.w, nH = tex.noise.h;

    const bFadeX = fadeAxis === 'right';
    // rotate90:纹理躺倒 —— 屏幕行喂纹理 u、屏幕列喂纹理 v(正常相反)。云纹的方向性
    // 纹理是竖版设计,宽扁容器(buff chip)转 90° 后扩散纹理才顺着水平方向走。
    // 速度偏移跟着纹理轴走(cox 恒为 u 轴偏移),视觉方向由实验室面板现场调。
    const rot = r.rotRef.current;

    for (let py = 0; py < h; py++) {
      const rowFade = bFadeX ? 1 : fadeLUT[py];
      const cRow = (py / h) * cPxPerU + (rot ? cox : coy);
      const nRow = (py / h) * nPxPerU + (rot ? nox : noy);

      for (let px = 0; px < w; px++) {
        const cCol = (px / w) * cPxPerU + (rot ? coy : cox);
        const cu = ((((rot ? cRow : cCol)) % cW) + cW) % cW;
        const cv = ((((rot ? cCol : cRow)) % cH) + cH) % cH;
        let lum = gammaLUT[cData[(cv | 0) * cW + (cu | 0)]];

        if (useNoise) {
          const nCol = (px / w) * nPxPerU + (rot ? noy : nox);
          const nu = ((((rot ? nRow : nCol)) % nW) + nW) % nW;
          const nv = ((((rot ? nCol : nRow)) % nH) + nH) % nH;
          const n = nData[(nv | 0) * nW + (nu | 0)] / 255;
          lum = (lum * (nFloor + p.noiseStrength * n)) | 0;
        }

        const fade = bFadeX ? fadeLUT[px] : rowFade;
        const i = (py * w + px) * 4;
        buf[i]     = r0;
        buf[i + 1] = g0;
        buf[i + 2] = b0;
        buf[i + 3] = (lum * fade) | 0;
      }
    }

    ctx.putImageData(imageData, 0, 0);
    timer = setTimeout(tick, TICK_MS);
  }

  tick();
}

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
  /** 淡出轴。'top'(默认,竖版卡)/ 'right'(宽扁 buff chip:右浓左淡,配合默认参数的向左飘)。 */
  fade?: SmokeFade;
  /** 纹理采样轴转 90°(竖版云纹躺倒给宽扁容器用,扩散方向顺水平轴)。 */
  rotate90?: boolean;
  /** 画布分辨率覆盖(默认 48×64 竖版)。宽扁容器传扁比例(如 96×24)更省也更贴合拉伸方向。 */
  resolution?: { w: number; h: number };
  /** 容器类覆盖(默认竖版卡的 absolute inset-0 rounded-lg)。 */
  className?: string;
}) {
  const p = params ?? DEFAULT_SMOKE_PARAMS;
  const [tex, setTex] = useState(textures);

  const paramsRef = useRef(p);
  paramsRef.current = p;
  const rarityRef = useRef(rarity);
  rarityRef.current = rarity;
  const texRef = useRef(tex);
  texRef.current = tex;
  const visibleRef = useRef(true);
  const fadeRef = useRef<SmokeFade>(fade);
  fadeRef.current = fade;
  const rotRef = useRef(rotate90);
  rotRef.current = rotate90;

  useEffect(() => { if (!tex) loadTextures().then(setTex); }, [tex]);

  const canvasRefCb = useCallback((canvas: HTMLCanvasElement | null) => {
    if (!canvas) return;
    startLoop(canvas, { paramsRef, rarityRef, texRef, visibleRef, fadeRef, rotRef });

    // Pause ticking when this card is scrolled off-screen / behind another
    // tooltip / hidden by display:none. The tick loop still beats but skips
    // the per-pixel work while invisible, so the cost approaches zero.
    if ('IntersectionObserver' in window) {
      const io = new IntersectionObserver(entries => {
        for (const e of entries) visibleRef.current = e.isIntersecting;
      }, { threshold: 0 });
      io.observe(canvas);
      // Stash the observer on the canvas so we can disconnect on unmount.
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      (canvas as any).__smoke_io = io;
    }
  }, []);

  useEffect(() => {
    return () => {
      // Cleanup is implicit: canvas.isConnected check inside the tick loop
      // stops the timer. We just need to disconnect the observer if any.
    };
  }, []);

  return (
    <canvas
      ref={canvasRefCb}
      width={resolution?.w ?? CW}
      height={resolution?.h ?? CH}
      className={className ?? 'absolute inset-0 w-full h-full rounded-lg pointer-events-none'}
    />
  );
}
