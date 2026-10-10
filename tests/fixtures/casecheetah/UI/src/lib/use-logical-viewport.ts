import { useEffect, useState } from 'react';
import { useUIStore } from '@/stores/ui-store';

/** CSS zoom 不改变媒体查询的视口；HUD 布局使用物理视口除以 UI 缩放。 */
export function useLogicalViewport() {
  const scale = useUIStore((state) => state.uiScale);
  const [viewport, setViewport] = useState(() => ({ width: window.innerWidth, height: window.innerHeight }));
  useEffect(() => {
    const resize = () => setViewport({ width: window.innerWidth, height: window.innerHeight });
    window.addEventListener('resize', resize);
    return () => window.removeEventListener('resize', resize);
  }, []);
  return { width: viewport.width / scale, height: viewport.height / scale };
}

/** 三个常驻 HUD 共用断点，避免面板间在缩放时各自换行。 */
export function useHudViewportClass() {
  const { width, height } = useLogicalViewport();
  return `${width <= 1000 ? 'hud-compact' : ''} ${width <= 800 ? 'hud-narrow' : ''} ${height <= 560 ? 'hud-short' : ''}`;
}
