/**
 * Renders a side-by-side preview of every color-blind mode listed in a
 * discrete setting's options. Each row shows the same palette filtered with
 * an SVG matrix tuned for that deficiency, so the player can compare
 * easily; the row matching the currently-selected option is highlighted.
 */

const SAMPLE_COLORS = [
  '#e74c3c', // red
  '#e67e22', // orange
  '#f1c40f', // yellow
  '#2ecc71', // green
  '#1abc9c', // teal
  '#3498db', // blue
  '#9b59b6', // purple
  '#ecf0f1', // off-white
];

/** Map an option label/id to one of our filter ids (or 'off' = no filter). */
function resolveFilterKey(label: string): string {
  const k = label.toLowerCase();
  if (k.includes('deuter')) return 'deuteranope';
  if (k.includes('protan')) return 'protanope';
  if (k.includes('tritan')) return 'tritanope';
  return 'off';
}

interface Props {
  options: string[];
  ids?: string[];
  value: number;
}

export function ColorBlindPreview({ options, ids, value }: Props) {
  return (
    <div className="settings-cb-preview">
      {/* Off-screen <defs> with the filter matrices.
          Matrices are simulation approximations sourced from the
          Brettel-Viénot research and used by most accessibility tools. */}
      <svg className="settings-cb-defs" aria-hidden="true">
        <defs>
          <filter id="cb-deuteranope" colorInterpolationFilters="sRGB">
            <feColorMatrix
              type="matrix"
              values="0.625 0.375 0    0 0
                      0.7   0.3   0    0 0
                      0     0.3   0.7  0 0
                      0     0     0    1 0"
            />
          </filter>
          <filter id="cb-protanope" colorInterpolationFilters="sRGB">
            <feColorMatrix
              type="matrix"
              values="0.567 0.433 0     0 0
                      0.558 0.442 0     0 0
                      0     0.242 0.758 0 0
                      0     0     0     1 0"
            />
          </filter>
          <filter id="cb-tritanope" colorInterpolationFilters="sRGB">
            <feColorMatrix
              type="matrix"
              values="0.95  0.05  0     0 0
                      0     0.433 0.567 0 0
                      0     0.475 0.525 0 0
                      0     0     0     1 0"
            />
          </filter>
        </defs>
      </svg>

      {options.map((opt, i) => {
        const key = resolveFilterKey(ids?.[i] ?? opt);
        const filter = key === 'off' ? 'none' : `url(#cb-${key})`;
        const isCurrent = i === value;
        return (
          <div key={i} className={`settings-cb-row ${isCurrent ? 'is-current' : ''}`}>
            <span className="settings-cb-label">{opt}</span>
            <div className="settings-cb-swatches" style={{ filter }}>
              {SAMPLE_COLORS.map((c, j) => (
                <span key={j} className="settings-cb-swatch" style={{ background: c }} />
              ))}
            </div>
          </div>
        );
      })}
    </div>
  );
}
