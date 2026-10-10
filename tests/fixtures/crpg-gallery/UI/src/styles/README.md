# UI Styles — Organization Guide

This directory holds all CSS for the CRPG WebUI. `UI/src/index.css` is a thin
entry point that imports the files here in the correct cascade order; every
other file under `styles/` is a focused concern.

The goal is to scale to dozens of UI features (inventory, HUD, combat, quests,
dialogue, crafting, shops, tutorials, character creation, …) without any
single file exceeding a few hundred lines or mixing unrelated concerns.

## Directory layout

```
styles/
├── core/           Foundation — always loaded first, used by everything.
│   ├── theme.css         @theme block (color palette, font stacks, design tokens)
│   ├── base.css          @layer base (html/body resets, global rules)
│   └── animations.css    Shared @keyframes + @utility animate-* (used across features)
│
├── primitives/     Reusable UI atoms (buttons, panels, tooltips, dialog frames).
│   (empty for now — create files here as shared components are built)
│
└── features/       Domain-specific styles. One file per major feature.
    └── subtitle.css      Cinematic subtitle overlay
```

## Where does new CSS go?

Decision tree:

1. **Is it a design token** (color, font family, spacing value reusable across the app)?
   → `core/theme.css`

2. **Is it a global rule** (html / body / body-level class)?
   → `core/base.css`

3. **Is it an animation / keyframe used by more than one feature**
   (e.g. a generic `fadeIn` usable anywhere)?
   → `core/animations.css`

4. **Is it a style for a UI primitive** that will be reused across features
   (e.g. the button style, the panel-with-gold-border style)?
   → `primitives/<name>.css` — create a new file

5. **Is it a style for a specific game feature** (subtitle, inventory item, HUD bar)?
   → `features/<feature>.css` — create a new file. Include feature-specific
   keyframes in the same file, not in `core/animations.css`.

## Naming conventions

- **Feature-scoped selectors**: prefix every class with the feature name.
  Example: `.subtitle-item`, `.subtitle-glow`, `.inventory-slot`, `.hud-health-bar`.
  This avoids cross-feature collisions without needing CSS Modules.

- **Feature-specific `@keyframes`**: prefix with the feature name too.
  Example: `@keyframes hp-shimmer`, `@keyframes inventory-pickup-pop`.
  Shared keyframes in `core/animations.css` don't need a prefix (e.g. `fadeIn`).

- **Feature-specific `@utility`**: same rule. `@utility animate-subtitle-pulse`
  stays with the subtitle feature; `@utility animate-shake` is generic and lives
  in `core/animations.css`.

## Import order (in `index.css`)

```css
@import "tailwindcss";

/* 1. Design tokens and base resets. Load before anything else. */
@import "./styles/core/theme.css";
@import "./styles/core/base.css";

/* 2. Shared animation library. */
@import "./styles/core/animations.css";

/* 3. Reusable UI primitives. */
/* @import "./styles/primitives/button.css"; — when needed */

/* 4. Feature-specific styles. Order inside this block doesn't usually matter
 *    because each feature is self-scoped via its prefix. */
@import "./styles/features/subtitle.css";

/* Tailwind source scan — can sit here or at the top; leaving at bottom
 * keeps the @import block clean and continuous. */
@source "../src/**/*.{tsx,ts,jsx,js}";
```

**CSS spec rule**: all `@import` statements must come before any other rule
(except `@charset` and `@layer` declarations). Splitting across files is fine,
but within any file `@import` is top-of-file only.

## Tailwind v4 notes

- `@theme` blocks in multiple files merge. Safe to keep the main one in `core/theme.css`
  and add feature-specific tokens inline in feature files if genuinely needed
  (usually avoid).

- `@layer base { … }` blocks in multiple files also merge into the single
  `base` layer.

- `@utility` declarations can live anywhere — Tailwind processes all imported files.

- `@source` directives tell Tailwind which files to scan for class names. Keep
  the main one in `index.css`; individual feature files typically don't need
  their own because their classes are used from matching `.tsx` components
  that `@source` already covers.

## Anti-patterns

- Don't co-locate `.css` next to a component (`SubtitleOverlay.css` next to
  `SubtitleOverlay.tsx`). We keep all styles under `styles/` so the full
  surface area is browseable in one place and Tailwind tooling has a single
  place to watch.

- Don't introduce CSS Modules. The prefix convention + Tailwind utilities
  already give us enough isolation without the `className={styles.xxx}`
  ceremony, and preserve easy Ctrl-F-ability for class names.

- Don't pile feature-specific keyframes into `core/animations.css`. It turns
  into a dumping ground fast. Keep keyframes with the feature that owns them
  unless they're genuinely reusable.
