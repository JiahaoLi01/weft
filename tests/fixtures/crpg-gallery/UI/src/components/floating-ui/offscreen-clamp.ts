/**
 * Off-screen edge clamp 算法 —— 用于 `edge-clamp` policy。
 *
 * 输入:instance 的 raw 投影坐标(x,y,可能落在 viewport 外)+ behindCamera flag + viewport 尺寸
 * 输出:贴在 viewport 边缘内侧 padding 处的 (x,y) + 箭头朝向角度
 *
 * 算法:
 *   1. 把"目标方向"用 viewport 中心 → instance.raw 坐标 表达,得到 unit vector
 *      - behindCamera=true 时 ProjectWorldLocationToScreen 给出的坐标"翻转"了(像 viewport
 *        中心对面),要把方向向量反向才正确指向 off-screen 的真实方向。
 *   2. 沿这条 unit vector 从 center 走到 viewport 矩形(留 padding)的内侧边缘交点,
 *      就是要 clamp 的位置。
 *   3. 箭头角度 = atan2(dy, dx),CSS rotation 直接吃 deg。
 *
 * 边界:viewport 任一维度 ≤ 0(还没收到 batch / 关游戏窗口瞬间),返回原坐标 + 0 角度,
 * 调用方自己决定要不要渲染。
 */

export interface ClampResult {
  /** 贴边后的 x,在 viewport 像素空间内,已留 padding */
  x: number;
  /** 贴边后的 y */
  y: number;
  /** 箭头朝向的 deg。0 = 向右,CSS rotation 习惯,顺时针为正。 */
  rotationDeg: number;
}

/**
 * @param rawX        instance 投影坐标 x(viewport 像素空间,**未** clamp)
 * @param rawY        instance 投影坐标 y
 * @param behindCamera C++ 端 `behindCamera` 字段(true → 投影翻转,要反向)
 * @param viewportW   viewport 宽度(像素)
 * @param viewportH   viewport 高度(像素)
 * @param padding     贴边时距 viewport 边的内侧 padding,默认 32px(给箭头本体留位置)
 */
export function clampToViewportEdge(
  rawX: number,
  rawY: number,
  behindCamera: boolean,
  viewportW: number,
  viewportH: number,
  padding = 32,
): ClampResult {
  if (viewportW <= 0 || viewportH <= 0) {
    return { x: rawX, y: rawY, rotationDeg: 0 };
  }

  const cx = viewportW / 2;
  const cy = viewportH / 2;

  // 方向向量。behindCamera 时反向 —— 翻转 ProjectWorldLocationToScreen 的镜像坐标。
  let dx = rawX - cx;
  let dy = rawY - cy;
  if (behindCamera) {
    dx = -dx;
    dy = -dy;
  }

  // 长度 0 兜底(instance 跟 camera 正好同位置):随便指右,UI 不会有什么意义,但不 crash
  const len = Math.hypot(dx, dy);
  if (len < 1e-3) {
    return { x: cx, y: cy, rotationDeg: 0 };
  }
  const ux = dx / len;
  const uy = dy / len;

  // 内侧矩形边界
  const minX = padding;
  const maxX = viewportW - padding;
  const minY = padding;
  const maxY = viewportH - padding;

  // 沿 unit vector 从 center 出发,求跟内侧矩形四条边的最近交点。参数化 t * unit + center,
  // 解 t >= 0 让 (cx + t*ux, cy + t*uy) 落在矩形边上。取 4 条边里最小正 t,即最先碰到的边。
  //
  // 等价于:对每条边算 t 候选,过滤负数和 NaN(平行的 axis),取最小。
  const candidates: number[] = [];
  if (ux > 0) candidates.push((maxX - cx) / ux);
  if (ux < 0) candidates.push((minX - cx) / ux);
  if (uy > 0) candidates.push((maxY - cy) / uy);
  if (uy < 0) candidates.push((minY - cy) / uy);

  const t = candidates.length > 0 ? Math.min(...candidates.filter((v) => v >= 0)) : 0;

  const clampedX = cx + ux * t;
  const clampedY = cy + uy * t;

  // 箭头朝向 = unit vector 的 atan2,转 deg。CSS rotate 默认 0=向右,跟数学定义一致。
  const rotationDeg = Math.atan2(uy, ux) * (180 / Math.PI);

  return { x: clampedX, y: clampedY, rotationDeg };
}
