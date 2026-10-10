import { useState } from 'react';
import { motion, AnimatePresence } from 'framer-motion';
import type { FloatingUIInstance } from '@/stores/floating-ui-store';
import { ABNORMAL_FLAG_ICONS, ABNORMAL_FLAG_FALLBACK, parseAbnormalFlags } from '@/config/abnormal-flags';
// Shared antique-gold frame language (config/battle-palette), identical across
// every HP tier so the bar reads as a single engraved plaque rather than a
// state-colored object. Only the fill hue carries the semantic — restrained,
// oil-painting tones, no neon. The in-panel HpBar (battle/HpBar.tsx) draws from
// the same palette so both HP readouts read as one instrument.
import { ANTIQUE_GOLD, BAR_GROUND, NAME_INK, brass, hpTierPalette } from '@/config/battle-palette';

interface HealthBarProps {
  instance: FloatingUIInstance;
}

const BAR_WIDTH = 108;
const BAR_HEIGHT = 10;
const BAR_RADIUS = 2;
const INNER_INSET = 1;
const INNER_WIDTH = BAR_WIDTH - INNER_INSET * 2;
const TICKS = [25, 50, 75] as const;

function toNumber(value: unknown, fallback: number): number {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

function clamp01(value: number): number {
  return Math.max(0, Math.min(1, value));
}

export function HealthBar({ instance }: HealthBarProps) {
  const d = instance.data;
  const cur = toNumber(d.CurrentHealth, 0);
  const max = toNumber(d.MaxHealth, 100);
  const pctRaw = d.HealthPercent != null ? toNumber(d.HealthPercent, 0) : (max > 0 ? cur / max : 0);
  const pct = clamp01(pctRaw);
  const name = String(d.DisplayName ?? '');
  const abnormalFlags = parseAbnormalFlags(d.AbnormalFlags);
  const isCritical = pct > 0 && pct < 0.3;
  const isDead = pct <= 0;

  const [previousPct, setPreviousPct] = useState(pct);
  const [trailPct, setTrailPct] = useState(pct);
  if (previousPct !== pct) {
    setTrailPct(previousPct);
    setPreviousPct(pct);
  }

  const palette = hpTierPalette(pct);
  const fillWidth = pct * INNER_WIDTH;
  const shouldShowDamageTrail = trailPct > pct;

  return (
    <motion.div
      className="flex flex-col items-center select-none pointer-events-none"
      animate={isCritical ? { scale: [1, 1.03, 1] } : { scale: 1 }}
      transition={{ duration: 1.3, repeat: isCritical ? Infinity : 0, ease: 'easeInOut' }}
      style={{
        minWidth: BAR_WIDTH,
        transformOrigin: 'center bottom',
        opacity: isDead ? 0.82 : 1,
      }}
    >
      {name && (
        <div
          className="relative mb-1 flex items-center justify-center leading-none"
          style={{
            maxWidth: 150,
            minHeight: 14,
            fontFamily: 'var(--font-default)',
          }}
        >
          {/* Feathered dark wash — a soft, FLAT OVAL halo with no hard edge.
              NB: we do NOT use `ellipse closest-side` — Ultralight's WebKit appears
              to ignore the size keyword and fall back to farthest-corner, which
              over-extends and reads as a fat round blob. Instead we use the default
              ellipse and end the gradient transparent at ~72%: with farthest-corner
              the box mid-sides sit at ~71% of the radius, so 72% reproduces the
              inscribed-oval look in a universally-supported way. A wide-and-short
              inset box makes the oval clearly elliptical (hugs a line of text). */}
          <span
            className="absolute pointer-events-none"
            style={{
              inset: '-2px -22px',
              background:
                'radial-gradient(ellipse at center, rgba(0,0,0,0.64) 0%, rgba(0,0,0,0.48) 34%, rgba(0,0,0,0.2) 56%, rgba(0,0,0,0) 72%)',
            }}
            aria-hidden
          />
          {/* Flanking hairlines — engraved-nameplate rules either side of the
              name (── Name ──), fading toward the outer ends. Victorian title-
              page idiom; brass to match the bar frame below. */}
          <span
            className="relative shrink-0"
            style={{
              width: 12,
              height: 1,
              marginRight: 5,
              background: `linear-gradient(90deg, ${brass(0)}, ${brass(0.6)})`,
            }}
            aria-hidden
          />
          <span
            className="relative truncate"
            style={{
              maxWidth: 112,
              color: NAME_INK,
              fontSize: 11,
              fontWeight: 600,
              letterSpacing: 0.4,
              textShadow: '0 1px 2px rgba(0,0,0,0.9), 0 0 2px rgba(0,0,0,0.7)',
            }}
          >
            {name}
          </span>
          <span
            className="relative shrink-0"
            style={{
              width: 12,
              height: 1,
              marginLeft: 5,
              background: `linear-gradient(270deg, ${brass(0)}, ${brass(0.6)})`,
            }}
            aria-hidden
          />
        </div>
      )}

      {/* Abnormal-status icons — sit between the name and the bar. Driven by
          instance.data.AbnormalFlags (comma-joined tag leaf names from the VM);
          empty / dead → no row. Bare gold glyphs with a tight dark silhouette,
          matching the inscription language above. */}
      {abnormalFlags.length > 0 && !isDead && (
        <div
          className="mb-1 flex items-center justify-center gap-1 pointer-events-none"
          aria-hidden
        >
          {abnormalFlags.map((flag) => {
            const Icon = ABNORMAL_FLAG_ICONS[flag] ?? ABNORMAL_FLAG_FALLBACK;
            return (
              <Icon
                key={flag}
                size={12}
                strokeWidth={1.5}
                color={NAME_INK}
                style={{
                  // Tiny offset silhouette for legibility over the world. If the
                  // Ultralight build ignores `filter`, the glyph still renders —
                  // just without the halo (safe degradation).
                  filter: 'drop-shadow(0 1px 0.5px rgba(0,0,0,0.95))',
                }}
              />
            );
          })}
        </div>
      )}

      <div
        className="relative overflow-hidden"
        style={{
          width: BAR_WIDTH,
          height: BAR_HEIGHT,
          background: BAR_GROUND,
          border: `1px solid ${ANTIQUE_GOLD}`,
          borderRadius: BAR_RADIUS,
          // Outer dark outline + tiny ground shadow for legibility over the
          // world; inner 1px channel reads as an engraved recess (flat, no bevel).
          boxShadow: [
            '0 0 0 1px rgba(8,6,3,0.85)',
            '0 1px 2px rgba(0,0,0,0.5)',
            'inset 0 0 0 1px rgba(8,6,3,0.6)',
          ].join(', '),
        }}
      >
        {shouldShowDamageTrail && (
          <motion.div
            key={`${trailPct}-${pct}`}
            className="absolute pointer-events-none"
            initial={{ width: trailPct * INNER_WIDTH, opacity: 0.85 }}
            animate={{ width: fillWidth, opacity: 0 }}
            transition={{
              width: { duration: 0.58, delay: 0.08, ease: 'easeOut' },
              opacity: { duration: 0.3, delay: 0.34, ease: 'easeOut' },
            }}
            style={{
              left: INNER_INSET,
              top: INNER_INSET,
              bottom: INNER_INSET,
              background: 'linear-gradient(90deg, rgba(221,192,121,0.5), rgba(192,114,106,0.62))',
            }}
            aria-hidden
          />
        )}

        <motion.div
          className="absolute overflow-hidden"
          initial={false}
          animate={{ width: fillWidth }}
          transition={{ duration: 0.28, ease: 'easeOut' }}
          // NB: no `will-change` here — a *permanent* will-change makes Ultralight 1.4
          // treat the fill as an ever-live composite layer reprocessed every frame
          // (the View never idles). framer-motion adds/removes it automatically for
          // the duration of the width animation, which is all we need.
          style={{
            left: INNER_INSET,
            top: INNER_INSET,
            bottom: INNER_INSET,
            background: palette.fill,
          }}
        >
          {/* Single 1px catch-light along the top — a drawn highlight line, not a glossy tube. */}
          <div
            className="absolute inset-x-0 top-0 pointer-events-none"
            style={{ height: 1, background: palette.lit, opacity: isDead ? 0 : 1 }}
            aria-hidden
          />
          {/* Leading-edge accent marking where current HP ends. */}
          <div
            className="absolute top-0 bottom-0 right-0 pointer-events-none"
            style={{ width: 1, background: palette.lit, opacity: isDead ? 0 : 0.85 }}
            aria-hidden
          />
        </motion.div>

        <AnimatePresence>
          {isCritical && (
            <motion.div
              initial={{ opacity: 0 }}
              animate={{ opacity: [0.3, 0.85, 0.3] }}
              exit={{ opacity: 0 }}
              transition={{ duration: 1.3, repeat: Infinity, ease: 'easeInOut' }}
              className="absolute inset-0 pointer-events-none"
              style={{
                borderRadius: BAR_RADIUS,
                boxShadow: 'inset 0 0 0 1px rgba(158,59,52,0.95), inset 0 0 5px rgba(158,59,52,0.45)',
              }}
              aria-hidden
            />
          )}
        </AnimatePresence>

        {/* Quarter graduations — fine engraved hairlines, like an antique gauge. */}
        {TICKS.map((tick) => (
          <div
            key={tick}
            className="absolute pointer-events-none"
            style={{
              left: `${tick}%`,
              top: INNER_INSET,
              bottom: INNER_INSET,
              width: 1,
              background: 'rgba(10,7,3,0.5)',
              boxShadow: '1px 0 0 rgba(231,221,191,0.12)',
            }}
            aria-hidden
          />
        ))}

        {isDead && (
          <div
            className="absolute inset-0 pointer-events-none"
            style={{
              background:
                'repeating-linear-gradient(45deg, rgba(12,9,5,0.62) 0 3px, rgba(40,34,25,0.55) 3px 6px)',
            }}
            aria-hidden
          />
        )}
      </div>
    </motion.div>
  );
}
