import { memo, useSyncExternalStore } from 'react';
import { AnimatePresence, motion } from 'framer-motion';
import type { FloatingUIInstance } from '@/stores/floating-ui-store';
import { getFloatingUITypeDef } from '@/config/floating-ui-types';

// The CRPG floating UI (the captured FloatingUIOverlay) as an engine drives it through Weft (docs/engine-communication.md,
// 13.4): the same instance components in the same wrapper, but the wrapper follows an anchor the engine writes every
// frame (data-anchor: centred on it, at its scale, hidden off screen, nearer ones on top), the engine sends an event
// when an instance comes or goes, and its view model fields through the view model "floating" (instance id to fields)
// when they change. React renders an instance when it comes and goes and when its fields change, never because it
// moved, and not because another one changed. An instance that goes fades where its anchor last was, as the original's
// does where its last batch put it.

type Anchored = FloatingUIInstance & { anchor: number };

const engine = (globalThis as any).weft.engine;
const fields = engine.model('floating');
const live = new Map<string, Anchored>();
let snapshot: Anchored[] = [];
let scheduled = false;
const listeners = new Set<() => void>();

// One render a frame, however many events and view model batches a delivery brings: the bridge runs microtasks after
// each of them (as a browser does between tasks), so publishing at once would render the overlay once for each. A
// View's frame runs its tasks after the delivery and before the animation callbacks, so the overlay renders before
// framer-motion's frame, as the original's does at its batch.
function publish() {
  if (scheduled) return;
  scheduled = true;
  setTimeout(() => {
    scheduled = false;
    snapshot = [...live.values()];
    for (const listener of listeners) listener();
  }, 0);
}

function subscribe(listener: () => void) {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

// Where the original's batch puts an instance is the anchor's here: the fields the components read are its id, type and
// data.
const placed = { x: 0, y: 0, scale: 1, visible: true, onScreen: true, behindCamera: false, inRange: true, zOrder: 0, distance: 0 };

// A delivery hands the page the view model before the events: an instance that comes takes its latest fields.
engine.on('floating.spawn', ({ id, type, anchor, data }: { id: string; type: string; anchor: number; data: Record<string, unknown> }) => {
  live.set(id, { ...placed, id, type, anchor, data: { ...data, ...(fields.value?.[id] ?? {}) } });
  publish();
});
engine.on('floating.despawn', ({ id }: { id: string }) => {
  if (live.delete(id)) publish();
});
fields.subscribe((_value: unknown, changed: Record<string, Record<string, unknown>>) => {
  let any = false;
  for (const [id, data] of Object.entries(changed)) {
    const instance = live.get(id);
    if (instance) {
      live.set(id, { ...instance, data });
      any = true;
    }
  }
  if (any) publish();
});

// The wrapper's fade, the same objects at every render.
const initial = { opacity: 0 };
const animate = { opacity: 1 };
const exit = { opacity: 0 };
const transition = { duration: 0.15, ease: 'easeOut' } as const;

// An instance: it renders again only when it is a new object (its fields changed), not when the list did.
const AnchoredInstance = memo(function AnchoredInstance({ instance }: { instance: Anchored }) {
  const Component = getFloatingUITypeDef(instance.type)!.component;
  return (
    <motion.div
      className="absolute top-0 left-0"
      data-anchor={String(instance.anchor)}
      data-anchor-pivot="50% 50%"
      data-anchor-offscreen="hide"
      data-anchor-missing="stay"
      initial={initial}
      animate={animate}
      exit={exit}
      transition={transition}
    >
      <Component instance={instance} />
    </motion.div>
  );
});

// presenceAffectsLayout off: by default AnimatePresence gives every instance a new presence context at each render, so
// that siblings with layout animations measure again, and every motion component of every instance renders again with
// it; the instances lie at their anchors and none animates its layout.
export function AnchoredFloatingUIOverlay() {
  const instances = useSyncExternalStore(subscribe, () => snapshot);
  return (
    <div className="absolute inset-0 overflow-hidden pointer-events-none" style={{ zIndex: 0 }}>
      <AnimatePresence presenceAffectsLayout={false}>
        {instances.map((instance) =>
          getFloatingUITypeDef(instance.type) ? <AnchoredInstance key={instance.id} instance={instance} /> : null,
        )}
      </AnimatePresence>
    </div>
  );
}
