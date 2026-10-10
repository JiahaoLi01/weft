import { useState, useCallback, useEffect, useMemo, type CSSProperties, type ReactNode } from 'react';
import { useFocusable, setFocus } from '@noriginmedia/norigin-spatial-navigation';
import { sendToUE, isInUnreal } from '@/lib/ue-bridge';
import { useSettingsStore, type SettingDef, type SettingsSchema } from '@/stores/settings-store';
import { useUIStore } from '@/stores/ui-store';
import { useT } from '@/stores/i18n-store';
import { useUIInputStore } from '@/stores/ui-input-store';
import { fkeyToGlyph } from '@/lib/input-glyphs';
import type { LocaleKeys } from '@/i18n';
import { SettingSlider } from '@/components/ui/SettingSlider';
import { SettingToggle } from '@/components/ui/SettingToggle';
import { SettingDropdown } from '@/components/ui/SettingDropdown';
import { ColorBlindPreview } from '@/components/ui/ColorBlindPreview';
import { renderRuby } from '@/lib/ruby';

/**
 * Compact "press [LB] / [RB] to cycle tabs" hint pill, rendered on the
 * sides of the tab strip. The displayed glyph follows two live signals:
 *   1. Which UI action ('tabPrev' / 'tabNext') the pill is for
 *   2. Whether the player is on gamepad or keyboard/mouse, per UE-pushed
 *      `uiInputDevice` updates
 * Both are pulled from `useUIInputStore`, so the pill re-renders on
 * device flips and on rebinds without any explicit refresh logic.
 *
 * Returns null until C++ has pushed bindings — avoids a flicker of
 * empty pills on first paint.
 */
function TabHint({ action }: { action: 'tabPrev' | 'tabNext' }) {
  // Pull both the binding and the controller family from the store. The
  // glyph helper picks the right family-specific dictionary (Xbox A vs
  // PlayStation Cross vs Switch B) based on whatever C++ HID detection
  // determined for the currently-connected pad.
  const glyph = useUIInputStore((s) => fkeyToGlyph(s.getKey(action), s.controllerFamily));
  if (!glyph) return <span className="settings-tab-hint settings-tab-hint--placeholder" />;
  return (
    <span className="settings-tab-hint" aria-hidden="true">
      <span className="settings-tab-hint-key">{glyph}</span>
    </span>
  );
}

/**
 * Focusable button — small wrapper that wires the spatial-nav `useFocusable`
 * hook to a `<button>` element. Used for the tabs, footer actions, and close
 * affordance, where the only navigation behavior we need is "Enter to fire".
 *
 * Adds `is-focused` to the className so the same focus ring rule applies
 * everywhere (see styles/features/settings.css).
 */
function FocusButton({
  className, onPress, ariaLabel, disabled, focusKey, children,
}: {
  className: string;
  onPress: () => void;
  ariaLabel?: string;
  disabled?: boolean;
  focusKey?: string;
  children: ReactNode;
}) {
  const { ref, focused } = useFocusable({
    onEnterPress: () => { if (!disabled) onPress(); },
    focusable: !disabled,
    focusKey,
  });
  return (
    <button
      ref={ref}
      type="button"
      className={`${className} ${focused ? 'is-focused' : ''}`}
      onClick={onPress}
      aria-label={ariaLabel}
      disabled={disabled}
    >
      {children}
    </button>
  );
}
import bgUrl from '@/assets/settings/background.png';
import leftPanelUrl from '@/assets/settings/left-panel.png';
import rightPanelUrl from '@/assets/settings/right-panel.png';

/**
 * Translate a SettingDef / SettingsCategory label. If `labelKey` resolves
 * to a real i18n entry, use that; otherwise fall back to the raw `label`
 * (whatever C++ sent, typically English).
 */
function useLocalizedLabel() {
  const t = useT();
  return useCallback(
    (labelKey: LocaleKeys | undefined, fallback: string): string => {
      if (!labelKey) return fallback;
      const translated = t(labelKey);
      return translated === labelKey ? fallback : translated;
    },
    [t],
  );
}

/** Dev-only mock schema so the layout renders when no UE backend is attached. */
function buildDevMockSchema(): SettingsSchema {
  return {
    video: {
      label: 'Video',
      labelKey: 'settings.category.video',
      settings: {
        _displayHeader: { type: 'subtitle', value: 0, label: '显示' },
        displayMode: {
          type: 'discrete', value: 1,
          options: ['窗口模式', '无边窗口', '全屏模式'],
          label: '显示模式',
          description: '选择游戏的显示模式',
        },
        resolution: {
          type: 'discrete', value: 0,
          options: ['2560×1440', '1920×1080', '1600×900'],
          label: '分辨率',
          description: '设置渲染分辨率，越高越清晰也越占用性能。',
        },
        refreshRate: {
          type: 'discrete', value: 0,
          options: ['60 Hz', '120 Hz', '144 Hz'],
          label: '刷新率',
        },
        vsync: { type: 'toggle', value: true, label: '垂直同步', description: '消除画面撕裂，但可能引入输入延迟。' },
        fpsCap: { type: 'scalar', value: 120, min: 30, max: 240, step: 10, label: '帧率上限' },

        _qualityHeader: { type: 'subtitle', value: 0, label: '画面' },
        viewDistance: { type: 'discrete', value: 2, options: ['低', '中', '高', '极高'], label: '视图距离' },
        postProcess:  { type: 'discrete', value: 2, options: ['低', '中', '高'], label: '后期处理' },
        shadow:       { type: 'discrete', value: 2, options: ['关', '低', '中', '高'], label: '阴影' },
        antiAlias:    { type: 'discrete', value: 1, options: ['关', 'TAA', 'TSR'], label: '抗锯齿' },
        gi:           { type: 'discrete', value: 1, options: ['关', 'Lumen 软件', 'Lumen 硬件'], label: '全局光照' },
        reflection:   { type: 'discrete', value: 1, options: ['屏幕空间', 'Lumen', '关'], label: '反射' },
        texture:      { type: 'discrete', value: 2, options: ['低', '中', '高', '极高'], label: '纹理' },

        _advancedHeader: { type: 'subtitle', value: 0, label: '高级' },
        motionBlur:   { type: 'toggle', value: false, label: '动态模糊' },
        dof:          { type: 'toggle', value: false, label: '景深' },
      },
    },
    audio: {
      label: 'Audio',
      labelKey: 'settings.category.audio',
      settings: {
        masterVolume: { type: 'scalar', value: 0.8, min: 0, max: 1, step: 0.05, label: '主音量' },
        musicVolume:  { type: 'scalar', value: 0.6, min: 0, max: 1, step: 0.05, label: '音乐' },
        sfxVolume:    { type: 'scalar', value: 0.9, min: 0, max: 1, step: 0.05, label: '音效' },
        voiceVolume:  { type: 'scalar', value: 0.85, min: 0, max: 1, step: 0.05, label: '人声' },
      },
    },
    input: {
      label: 'Game',
      labelKey: 'settings.category.input',
      settings: {
        invertY:      { type: 'toggle', value: false, label: '反转 Y 轴' },
        mouseSens:    { type: 'scalar', value: 0.5, min: 0, max: 1, step: 0.05, label: '鼠标灵敏度' },
        sprintMode:   {
          type: 'discrete',
          value: 0,
          options: ['Hold', 'Toggle'],
          label: 'Sprint Mode',
          description: 'Whether the sprint key is held down or toggled with a single press.',
        },
      },
    },
    accessibility: {
      label: 'Accessibility',
      labelKey: 'settings.category.accessibility',
      settings: {
        colorBlindMode: {
          type: 'discrete', value: 0,
          options: ['Off', 'Deuteranope', 'Protanope', 'Tritanope'],
          label: 'Color Blind Mode',
          description: 'Color correction for color vision deficiency.',
        },
        colorBlindStrength: {
          type: 'scalar', value: 1.0, min: 0, max: 1, step: 0.05,
          label: 'Color Blind Strength',
        },
      },
    },
  };
}

export function Settings() {
  const schema = useSettingsStore(s => s.schema);
  const values = useSettingsStore(s => s.values);
  const isDirty = useSettingsStore(s => s.isDirty);
  const { setValue, apply, cancel, loadSchema } = useSettingsStore.getState();
  const goBack = useUIStore(s => s.goBack);

  const t = useT();
  const localize = useLocalizedLabel();

  const [activeTab, setActiveTab] = useState<string | null>(null);
  const [hoveredKey, setHoveredKey] = useState<string | null>(null);
  // 关闭时设true → 添加.is-closing类跑240ms淡出 → onAnimationEnd里再goBack()。
  // 重复触发(连按Esc / 同时点X)被isClosing门挡住，避免动画过程中页面突然消失。
  const [isClosing, setIsClosing] = useState(false);

  // Request settings data from C++ on first mount. In dev mode (no UE)
  // or if no schema arrives within a short window, fall back to a mock so
  // the layout remains testable in the browser preview.
  useEffect(() => {
    if (schema) return;
    if (isInUnreal()) sendToUE('request_settings');
    const fallback = window.setTimeout(() => {
      if (!useSettingsStore.getState().schema) {
        loadSchema(buildDevMockSchema());
      }
    }, 500);
    return () => window.clearTimeout(fallback);
  }, [schema, loadSchema]);

  const handleApply = useCallback(() => apply(), [apply]);
  // 启动淡出。值的cancel + 栈的goBack要等动画结束(onAnimationEnd)，
  // 否则React立刻卸载，CSS动画来不及跑就被unmount。
  const handleCancel = useCallback(() => {
    if (isClosing) return;
    setIsClosing(true);
  }, [isClosing]);
  const handleCloseAnimationEnd = useCallback(() => {
    if (!isClosing) return;
    cancel();
    goBack();
  }, [isClosing, cancel, goBack]);

  // LB / RB cycle through category tabs even when focus is deep in the
  // list — driven by the `ui:tabPrev` / `ui:tabNext` CustomEvents that
  // `useGamepadInput` emits on UE-forwarded TabPrev / TabNext actions.
  // We compute the new tab from the current schema's category order so
  // the cycling stays correct as categories are added / removed.
  //
  // ── Why we re-anchor focus on the new tab ────────────────────────
  //
  // If the player was focused on a row in the body when they pressed
  // the shoulder button, switching activeTab causes React to unmount
  // the entire body list and render the new category's rows. The old
  // row's focusable disappears from spatial-nav's registry — but the
  // lib's currentFocusKey still points at it. Subsequent D-pad presses
  // then have no live anchor to navigate from and silently do nothing.
  // Defer setFocus by one frame so the new tab's body has had a chance
  // to mount its rows; that way pressing D-pad Down right after a
  // shoulder switch lands on the first row of the new category.
  useEffect(() => {
    if (!schema) return;
    const handler = (delta: 1 | -1) => () => {
      const keys = Object.keys(schema);
      if (keys.length === 0) return;
      const current = activeTab ?? keys[0];
      const idx = keys.indexOf(current);
      const next = keys[(idx + delta + keys.length) % keys.length];
      setActiveTab(next);
      setHoveredKey(null);
      requestAnimationFrame(() => setFocus(`settings-tab-${next}`));
    };
    const onPrev = handler(-1);
    const onNext = handler(1);
    window.addEventListener('ui:tabPrev', onPrev);
    window.addEventListener('ui:tabNext', onNext);
    return () => {
      window.removeEventListener('ui:tabPrev', onPrev);
      window.removeEventListener('ui:tabNext', onNext);
    };
  }, [schema, activeTab]);

  // Seed the initial spatial-navigation focus on every Settings mount.
  // The norigin lib won't auto-pick a focusable on first arrow press;
  // without this, the player's first D-pad / arrow input goes nowhere.
  // We re-seed on every mount (not just when `schema` first arrives)
  // because the page is push/pop'd by the UI stack: the second time the
  // player opens settings, schema is already cached, so a `[schema]` dep
  // wouldn't re-run.
  useEffect(() => {
    if (!schema) return;
    const firstKey = Object.keys(schema)[0];
    if (!firstKey) return;
    // Defer one frame so the FocusButton mount effects have run and
    // registered the per-tab focus keys with the spatial-nav lib.
    const id = requestAnimationFrame(() => setFocus(`settings-tab-${firstKey}`));
    return () => cancelAnimationFrame(id);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [!!schema]);

  // Keyboard shortcuts: F5 apply, Esc cancel
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'F5') {
        e.preventDefault();
        if (isDirty) handleApply();
      } else if (e.key === 'Escape') {
        e.preventDefault();
        handleCancel();
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [handleApply, handleCancel, isDirty]);

  const hoveredDef = useMemo<SettingDef | null>(() => {
    if (!hoveredKey || !schema) return null;
    for (const cat of Object.values(schema)) {
      if (hoveredKey in cat.settings) return cat.settings[hoveredKey];
    }
    return null;
  }, [hoveredKey, schema]);

  const hoveredValue = hoveredKey ? values[hoveredKey] ?? hoveredDef?.value : undefined;

  const pageStyle = {
    '--settings-bg-img':    `url(${bgUrl})`,
    '--settings-left-img':  `url(${leftPanelUrl})`,
    '--settings-right-img': `url(${rightPanelUrl})`,
  } as CSSProperties;

  if (!schema) {
    return (
      <div
      className={`settings-page ${isClosing ? 'is-closing' : ''}`}
      style={pageStyle}
      onAnimationEnd={handleCloseAnimationEnd}
    >
        <div className="settings-bg" />
        <div className="settings-waiting">{t('settings.waiting_data')}</div>
      </div>
    );
  }

  const categories = Object.entries(schema);
  const currentTab = activeTab ?? categories[0]?.[0] ?? '';
  const currentCategory = schema[currentTab];

  return (
    <div
      className={`settings-page ${isClosing ? 'is-closing' : ''}`}
      style={pageStyle}
      onAnimationEnd={handleCloseAnimationEnd}
    >
      <div className="settings-bg" />

      <div className="settings-shell">
        {/* ── Header ─────────────────────────────────────────────────
         *
         * Layout: hint pill | centered tab strip | hint pill | close
         *
         * The decorative "M" logo is gone — it carried no information and
         * it pushed the tabs off-center. The two hint pills flanking the
         * tabs render the LB / RB glyphs for the player's current input
         * device (gamepad shows "LB" / "RB", keyboard shows "Q" / "E"),
         * making the "shoulders cycle tabs" affordance discoverable
         * without any tutorial text. Glyphs come from the live UE binding
         * map so a rebind in the C++-side IMC is reflected here without
         * touching the Web UI. */}
        <header className="settings-header">
          <TabHint action="tabPrev" />
          <nav className="settings-tabs">
            {categories.map(([key, cat]) => (
              <FocusButton
                key={key}
                // Every tab gets a stable focusKey derived from its category
                // id. The mount-seed effect below seeds focus on the first
                // one; the LB / RB shoulder handler below uses the new
                // active tab's focusKey to re-anchor focus on the tab strip
                // after a switch — without this, if focus was deep in the
                // (now-unmounted) body list, spatial-nav would have no live
                // anchor to navigate from on the next D-pad press.
                focusKey={`settings-tab-${key}`}
                className={`settings-tab ${key === currentTab ? 'is-active' : ''}`}
                onPress={() => {
                  setActiveTab(key);
                  setHoveredKey(null);
                }}
              >
                {localize(cat.labelKey, cat.label)}
              </FocusButton>
            ))}
          </nav>
          <TabHint action="tabNext" />
          <FocusButton
            className="settings-close"
            onPress={handleCancel}
            ariaLabel={t('settings.cancel')}
          >
            ×
          </FocusButton>
        </header>

        {/* ── Body ───────────────────────────────────────────────── */}
        <div className="settings-body">
          {/* Left panel — settings list */}
          <aside className="settings-panel settings-panel-left">
            <div className="settings-panel-inner">
              <div className="settings-list">
                {currentCategory && Object.entries(currentCategory.settings).map(([key, def]) => {
                  const val = values[key] ?? def.value;

                  if (def.visibleWhen) {
                    const depVal = values[def.visibleWhen.key];
                    if (depVal !== def.visibleWhen.equals) return null;
                  }

                  let isDisabled = false;
                  if (def.enabledWhen) {
                    const depVal = values[def.enabledWhen.key];
                    isDisabled = depVal !== def.enabledWhen.equals;
                  }

                  const localizedDef: SettingDef = {
                    ...def,
                    label: localize(def.labelKey, def.label),
                    description: def.descriptionKey
                      ? localize(def.descriptionKey, def.description ?? '')
                      : def.description,
                    options: def.optionsKey
                      ? def.optionsKey.map((k, i) => localize(k, def.options?.[i] ?? ''))
                      : def.options,
                    optionDescriptions: def.optionDescriptionsKey
                      ? def.optionDescriptionsKey.map((k, i) => localize(k, def.optionDescriptions?.[i] ?? ''))
                      : def.optionDescriptions,
                  };

                  if (def.type === 'subtitle') {
                    return (
                      <div key={key} className="settings-section-header">
                        <span>{localizedDef.label}</span>
                      </div>
                    );
                  }

                  const isActive = hoveredKey === key;
                  const wrapper = isDisabled ? 'is-disabled' : '';

                  if (def.type === 'scalar') {
                    return (
                      <div key={key} className={wrapper}>
                        <SettingSlider
                          settingKey={key} def={localizedDef}
                          value={val as number} isActive={isActive}
                          onChange={setValue} onHover={setHoveredKey}
                        />
                      </div>
                    );
                  }
                  if (def.type === 'toggle') {
                    return (
                      <div key={key} className={wrapper}>
                        <SettingToggle
                          settingKey={key} def={localizedDef}
                          value={val as boolean} isActive={isActive}
                          onChange={setValue} onHover={setHoveredKey}
                        />
                      </div>
                    );
                  }
                  if (def.type === 'discrete') {
                    return (
                      <div key={key} className={wrapper}>
                        <SettingDropdown
                          settingKey={key} def={localizedDef}
                          value={val as number} isActive={isActive}
                          onChange={setValue} onHover={setHoveredKey}
                        />
                      </div>
                    );
                  }
                  return null;
                })}
              </div>
            </div>
          </aside>

          {/* Right panel — detail */}
          <section className="settings-panel settings-panel-right">
            <div className="settings-panel-inner">
              <div className="settings-detail">
                {hoveredDef ? (
                  <DetailContent def={hoveredDef} value={hoveredValue} localize={localize} />
                ) : (
                  <div className="settings-detail-empty">{t('settings.detail_placeholder')}</div>
                )}
              </div>
            </div>
          </section>
        </div>

        {/* ── Footer ─────────────────────────────────────────────── */}
        <footer className="settings-footer">
          <FocusButton
            className="settings-action is-primary"
            onPress={handleApply}
            disabled={!isDirty}
          >
            <span className="settings-action-key">F5</span>
            {t('settings.apply')}
          </FocusButton>
          <FocusButton
            className="settings-action"
            onPress={handleCancel}
          >
            <span className="settings-action-key">Esc</span>
            {t('settings.cancel')}
          </FocusButton>
        </footer>
      </div>
    </div>
  );
}

interface DetailContentProps {
  def: SettingDef;
  value: unknown;
  localize: (key: LocaleKeys | undefined, fallback: string) => string;
}

function DetailContent({ def, value, localize }: DetailContentProps) {
  const title = localize(def.labelKey, def.label);
  const description = def.descriptionKey
    ? localize(def.descriptionKey, def.description ?? '')
    : def.description;

  const hasCustomPreview = def.previewKind === 'colorblind';

  return (
    <>
      <h2 className="settings-detail-title">{title}</h2>
      {description && <div className="settings-detail-subtitle">{description}</div>}
      <div className="settings-detail-divider" />

      {def.media?.type === 'image' && (
        <img className="settings-detail-media" src={def.media.url} alt="" />
      )}
      {def.media?.type === 'video' && (
        <video className="settings-detail-media" src={def.media.url} autoPlay loop muted />
      )}

      {def.previewKind === 'colorblind' && def.type === 'discrete' && def.options && (
        <ColorBlindPreview
          options={def.options}
          ids={def.ids}
          value={typeof value === 'number' ? value : (def.value as number)}
        />
      )}

      {!hasCustomPreview && def.type === 'discrete' && def.options && def.options.length > 0 && (
        <div className="settings-detail-options">
          {def.options.map((opt, i) => {
            // Live-translate via optionsKey / optionDescriptionsKey when
            // present; falls back to the static strings otherwise. This is
            // the same translation strategy as label/description above so
            // the panel reads correctly after a language switch without a
            // schema reload.
            const name = def.optionsKey
              ? localize(def.optionsKey[i], opt)
              : opt;
            const rawDesc = def.optionDescriptions?.[i];
            const desc = def.optionDescriptionsKey
              ? localize(def.optionDescriptionsKey[i], rawDesc ?? '')
              : rawDesc;
            return (
              <div key={i} className="settings-detail-option-row">
                <span className="settings-detail-option-bullet" />
                <div>
                  <span className="settings-detail-option-name">{name}</span>
                  {desc && (
                    // Run option descriptions through the ruby parser so an
                    // author can include `[漢字]{かん|じ}` syntax inline. For
                    // settings whose meaning is hard to convey in prose
                    // (e.g. furigana density), the rendered example sentence
                    // is the demo. Plain descriptions just pass through.
                    <span className="settings-detail-option-desc">
                      {renderRuby(desc)}
                    </span>
                  )}
                </div>
              </div>
            );
          })}
        </div>
      )}
    </>
  );
}
