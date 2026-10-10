import React from 'react';

/**
 * 九宫格（HierarchyUI.md 第 9 节）：a painted frame that keeps its brush edges at any size. Ultralight draws CSS
 * `border-image` wrongly (slices misplaced), so the nine pieces are plain clipped images: the corners at their drawn size,
 * the edges stretched one way, the centre both ways. Each piece holds the whole source image, scaled and offset in
 * percentages of the piece, so it needs no measuring.
 *
 * `slice` is cut from the source image (px: top, right, bottom, left); `edge` is how thick each side is drawn (CSS px).
 * A side cut at 0 has no piece: the centre runs to that edge (a docked side).
 */
export type NineSliceArt = { src: string; size: [number, number]; slice: [number, number, number, number]; edge: [number, number, number, number] };

/** The painted panels of UI/public/art, cut for nine-slicing (measured from their alpha). */
export const panelArt = {
  /** Four brushed sides: floating cards, the dossier, the view strip. */
  card: { src: 'art/ink-panel-v2.png', size: [1024, 1536], slice: [130, 110, 190, 200], edge: [30, 26, 42, 46] } as NineSliceArt,
  /** Docked on the right: only the left side brushed (the business tree). */
  dockRight: { src: 'art/ink-dock-right-v1.png', size: [1024, 1536], slice: [0, 0, 0, 230], edge: [0, 0, 0, 52] } as NineSliceArt,
  /** Docked on the left: only the right side brushed (the place's archive). The ink ends near column 912. */
  dockLeft: { src: 'art/ink-dock-left-v1.png', size: [1024, 1536], slice: [0, 230, 0, 0], edge: [0, 52, 0, 0] } as NineSliceArt,
};

export function NineSlice({ art, className = '', children, ...rest }: { art: NineSliceArt; className?: string; children?: React.ReactNode } & React.HTMLAttributes<HTMLDivElement>) {
  const { src, size: [w, h], slice: [t, r, b, l], edge: [et, er, eb, el] } = art;
  // [source from, source to, left / top, width / height] for each column and row.
  const cols: [number, number, string, string][] = [[0, l, '0', `${el}px`], [l, w - r, `${el}px`, `calc(100% - ${el + er}px)`], [w - r, w, `calc(100% - ${er}px)`, `${er}px`]];
  const rows: [number, number, string, string][] = [[0, t, '0', `${et}px`], [t, h - b, `${et}px`, `calc(100% - ${et + eb}px)`], [h - b, h, `calc(100% - ${eb}px)`, `${eb}px`]];
  const pieces: React.ReactNode[] = [];
  rows.forEach(([sy0, sy1, top, height], ri) => cols.forEach(([sx0, sx1, left, width], ci) => {
    if (sx1 <= sx0 || sy1 <= sy0) return;
    const sw = sx1 - sx0, sh = sy1 - sy0;
    pieces.push(<i key={ri * 3 + ci} className="slice" style={{ left, top, width, height }}>
      <img src={src} alt="" draggable={false} style={{ width: `${(w / sw) * 100}%`, height: `${(h / sh) * 100}%`, left: `${(-sx0 / sw) * 100}%`, top: `${(-sy0 / sh) * 100}%` }} />
    </i>);
  }));
  return <div className={`nine-slice ${className}`} {...rest}>
    <div className="nine-slice-art" aria-hidden="true">{pieces}</div>
    <div className="nine-slice-content">{children}</div>
  </div>;
}
