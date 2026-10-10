import { useEffect } from "react";
import { useSubtitleStore } from "@/stores/subtitle-store";
import { getSpeakerName, getSpeakerColor } from "@/config/speakers";
import { onUECall } from "@/lib/ue-bridge";
import { useI18nStore } from "@/stores/i18n-store";
import { renderRuby } from "@/lib/ruby";

/**
 * Cinematic subtitle overlay — "soft glow" variant.
 *
 * Per art direction: the subtitle text appears instantly (movie-caption
 * feel). Behind it, a soft radial-gradient glow fades in/out. On exit,
 * the text also fades out alongside the glow.
 *
 * ── DOM pooling (why nothing ever unmounts) ───────────────────────────
 *
 * The overlay renders a FIXED pool of `.subtitle-item` slots (one per index
 * in the store's `slots` array) that are mounted once and never removed.
 * `show`/`hide` only assign or null-out a slot; an idle (null) slot stays in
 * the DOM as `display:none`.
 *
 * Why: Ultralight's paint pipeline hitches on the DOM-node *removal* mutation.
 * The previous design unmounted each item on exit and dropped a frame in-engine
 * on fade-out (fade-IN, a mount, was always smooth). It also needed a
 * `subtitle-root-keepalive` brightness-filter hack to mask a frozen "ghost"
 * frame at the removal moment. With a fixed pool:
 *
 *   - Nothing ever unmounts → no removal mutation → no fade-out hitch.
 *   - The `.subtitle-glow` (with `filter: blur(10px)`) is created once and
 *     never torn down → no per-exit filter-layer teardown.
 *   - Idle slots are `display:none`, applied only once the slot has already
 *     faded to opacity:0 — so even if Ultralight froze that frame it would be
 *     blank, never a ghost. The keepalive hack is gone entirely.
 *
 * Visual stacking order is driven by CSS `order` (the entry's `seq`), so newer
 * subtitles sit below older ones WITHOUT moving DOM nodes between indices.
 *
 * Multi-line support: `.subtitle-text` has `white-space: pre-wrap` so explicit
 * `\n` renders as line breaks; long lines wrap inside `max-width: 80%`.
 *
 * C++ sends:
 *   CallUI("showSubtitle", { id, content, speaker, duration, isSoundAssistant })
 *   CallUI("hideSubtitle", { id })
 *   CallUI("hideAllSubtitles", {})
 *
 * Settings (persisted in localStorage):
 *   showSpeakerName — prefix "Speaker: " before content
 *   showSpeakerColor — colorize speaker name
 *   fontScale — text size multiplier
 *   bgOpacity — glow strength (0 = invisible glow, 1 = reference look)
 */

export function SubtitleOverlay() {
  const slots = useSubtitleStore((s) => s.slots);
  const settings = useSubtitleStore((s) => s.settings);
  const show = useSubtitleStore((s) => s.show);
  const hide = useSubtitleStore((s) => s.hide);
  const hideAll = useSubtitleStore((s) => s.hideAll);
  const locale = useI18nStore((s) => s.locale);

  useEffect(() => {
    const unsub1 = onUECall("showSubtitle", (data: unknown) => {
      const d = data as {
        id: string;
        content: string;
        speaker: string;
        duration?: number;
        isSoundAssistant?: boolean;
      };
      show(d);
    });
    const unsub2 = onUECall("hideSubtitle", (data: unknown) => {
      const d = data as { id: string };
      hide(d.id);
    });
    const unsub3 = onUECall("hideAllSubtitles", () => {
      hideAll();
    });
    return () => {
      unsub1();
      unsub2();
      unsub3();
    };
  }, [show, hide, hideAll]);

  const baseFontSize = 18 * settings.fontScale;

  // Root container is PERMANENTLY MOUNTED. `lang={locale}` lets browser-level
  // features (line-break, shaping) match the active language. `data-furigana-
  // mode` drives the CSS visibility filter on <rt> elements.
  return (
    <div
      lang={locale}
      data-furigana-mode={settings.furiganaMode}
      className="subtitle-root fixed bottom-0 left-0 right-0 z-50 flex flex-col items-center gap-2 pb-[12%] pointer-events-none"
    >
      {slots.map((entry, i) => {
        // Build the text line for an occupied slot; idle slots render empty
        // (but the divs stay mounted — that's the whole point of pooling).
        let textContent: React.ReactNode = null;
        if (entry) {
          const speakerName = getSpeakerName(entry.speaker, locale);
          const speakerColor = getSpeakerColor(entry.speaker);
          const rendered = renderRuby(entry.content);

          if (entry.isSoundAssistant) {
            // SDH lines wrap the raw content with literal brackets for the
            // accessibility convention; ruby still parsed inside for the rare
            // annotated sound description.
            textContent = <span className="subtitle-sdh">[{rendered}]</span>;
          } else if (settings.showSpeakerName && entry.speaker) {
            textContent = (
              <>
                <span
                  style={{
                    color: settings.showSpeakerColor ? speakerColor : undefined,
                  }}
                >
                  {speakerName}
                </span>
                <span style={{ opacity: 0.5, margin: "0 0.4em" }}>:</span>
                <span>{rendered}</span>
              </>
            );
          } else {
            textContent = rendered;
          }
        }

        // Stable key = slot index → React never unmounts these nodes; it only
        // updates their content/state. `order` (entry seq) controls visual
        // stacking by recency without moving DOM nodes.
        return (
          <div
            key={i}
            className="subtitle-item"
            data-state={entry ? entry.state : "idle"}
            style={{ order: entry ? entry.seq : 0 }}
          >
            <div
              className="subtitle-glow"
              style={{ ["--bg-opacity" as string]: settings.bgOpacity }}
            />

            <div
              className="subtitle-text"
              style={{ fontSize: `${baseFontSize}px` }}
            >
              {textContent}
            </div>
          </div>
        );
      })}
    </div>
  );
}
