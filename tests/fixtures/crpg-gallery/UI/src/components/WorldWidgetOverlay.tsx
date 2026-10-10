import { useWorldWidgetStore } from '@/stores/world-widget-store';
import { useSettingsStore } from '@/stores/settings-store';
import { WORLD_WIDGET_TYPES } from '@/config/world-widget-types';

/**
 * Renders all active world-space widgets on an always-on, full-screen overlay.
 *
 * Mount once in App.tsx, outside the page stack — these widgets belong to the
 * game world, not to any particular UI screen. Individual widgets control their
 * own visibility via `widget.visible`; we render them all and toggle display
 * so CSS animations can fire on hide.
 *
 * Coordinate chain (must stay in sync with UWorldWidgetController::FlushWebBatch):
 *   UE logical viewport px × DPIScale_UE (applied in C++) = CSS px
 *   CSS px × html.zoom (= uiScale) = visual screen px
 * We want visual screen px to match the original logical viewport coord, so we
 * divide the incoming CSS coord by uiScale here to cancel the html.zoom.
 */
export function WorldWidgetOverlay() {
  const widgets = useWorldWidgetStore(s => s.widgets);
  const uiScale = useSettingsStore(s => s.uiScale);
  const inv = uiScale > 0 ? 1 / uiScale : 1;

  return (
    <div
      className="absolute inset-0 overflow-hidden pointer-events-none"
      style={{ zIndex: 5 }}
    >
      {widgets.map(w => {
        const Component = WORLD_WIDGET_TYPES[w.type];
        if (!Component) return null;
        return (
          <div
            key={w.id}
            className="absolute top-0 left-0"
            style={{
              transform: `translate3d(${w.x * inv}px, ${w.y * inv}px, 0) translate(-50%, -50%) scale(${w.scale})`,
              display: w.visible ? 'block' : 'none',
              zIndex: w.zOrder,
            }}
          >
            <Component widget={w} />
          </div>
        );
      })}
    </div>
  );
}
