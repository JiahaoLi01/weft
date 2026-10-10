/**
 * English locale — must satisfy the same key set as zh-CN.
 */
import type { LocaleKeys } from './zh-CN';

const en: Record<LocaleKeys, string> = {
  // ── Main Menu ──────────────────────────────────────────────
  'menu.badge':         'CRPG Framework',
  'menu.title.line1':   'Purple',
  'menu.title.line2':   'Scheme',
  'menu.tagline':       'A tale of intrigue, ambition, and the shadows that shape destiny.',

  'menu.start_game':          'Start Game',
  'menu.start_game.subtitle': 'BEGIN YOUR JOURNEY',
  'menu.continue':            'Continue',
  'menu.continue.subtitle':   'LOAD LATEST SAVE',
  'menu.settings':            'Settings',
  'menu.settings.subtitle':   'CONFIGURE OPTIONS',
  'menu.exit':                'Exit',
  'menu.exit.subtitle':       'LEAVE THE REALM',

  'menu.footer.build':      'BUILD',
  'menu.footer.engine':     'ENGINE',
  'menu.footer.framework':  'FRAMEWORK',

  // ── HUD ────────────────────────────────────────────────────
  'hud.not_connected': 'Not connected to UE',

  // ── Settings — shell ───────────────────────────────────────
  'settings.title':              'Settings',
  'settings.apply':              'Apply',
  'settings.cancel':             'Cancel',
  'settings.waiting_data':       'Waiting for settings data…',
  'settings.detail_placeholder': 'Hover a setting to see details',

  // ── Settings — categories ──────────────────────────────────
  'settings.category.interface':     'Interface',
  'settings.category.audio':         'Audio',
  'settings.category.video':         'Video',
  'settings.category.input':         'Input',
  'settings.category.accessibility': 'Accessibility',

  // ── Settings — Interface (client-only) ─────────────────────
  'settings.label.language':        'Language',
  'settings.desc.language':         'Change the display language of the interface.',
  'settings.label.ui_font':         'UI Font',
  'settings.desc.ui_font':          'Font used for menus and body text.',
  'settings.label.display_font':    'Display Font',
  'settings.desc.display_font':     'Font used for titles and emphasized headings.',
  'settings.label.subtitle_font':   'Subtitle Font',
  'settings.desc.subtitle_font':    'Font used for subtitles. The mixed face renders CJK with Noto Serif SC and digits/Latin with Cinzel Decorative.',

  // ── Settings — Subtitle (client-only, injected into accessibility) ─
  'settings.subtitle.header':             'Subtitles',
  'settings.label.show_speaker_name':     'Show Speaker Name',
  'settings.desc.show_speaker_name':      'Display the speaker\'s name before subtitle text (e.g. "Aldric: Hello").',
  'settings.label.show_speaker_color':    'Show Speaker Color',
  'settings.desc.show_speaker_color':     'Colorize the speaker name with their character color.',
  'settings.label.subtitle_font_scale':   'Subtitle Font Size',
  'settings.desc.subtitle_font_scale':    'Scale subtitle text size (1.0 = default).',
  'settings.label.subtitle_bg_opacity':   'Subtitle Background',
  'settings.desc.subtitle_bg_opacity':    'Background darkness behind subtitle text (0 = transparent, 1 = opaque).',

  // Furigana density — Japanese-only setting (visibleWhen language === 'ja')
  'settings.label.furigana_mode':         'Furigana',
  'settings.desc.furigana_mode':          'Controls how often the kana reading appears above kanji in subtitles.',
  'settings.option.furigana.off':         'Kanji only',
  'settings.option.furigana.key':         'Key words only',
  'settings.option.furigana.all':         'All readings',
  'settings.option.furigana.off.desc':    'Hide every reading. Example: "魔法で異世界に来た。"',
  'settings.option.furigana.key.desc':    'Only readings the author marked as key. Example: "[魔法]!{まほう}で異世界に来た。"',
  'settings.option.furigana.all.desc':    'Every reading the script provides. Example: "[魔法]!{まほう}で[異世界]{い|せ|かい}に来た。"',

  // ── Settings — Accessibility (extensions) ──────────────────
  'settings.label.ui_scale':        'UI Scale',
  'settings.desc.ui_scale':         'Scale all UI elements. Higher values make text and controls larger.',

  // ── Settings — Input / Controls ────────────────────────────
  'settings.label.sprint_mode':         'Sprint Mode',
  'settings.desc.sprint_mode':          'Whether the sprint key is held down or toggled with a single press.',
  'settings.option.sprint_mode.hold':   'Hold',
  'settings.option.sprint_mode.toggle': 'Toggle',
  'settings.option.sprint_mode.hold.desc':   'Sprint while the key is held; release to stop.',
  'settings.option.sprint_mode.toggle.desc': 'Tap the key to start sprinting; tap again to stop.',

  // ── Settings — Market (client-only, injected into accessibility) ─
  'settings.market.header':                       'Market',
  'settings.label.market_color':                  'Price Colors',
  'settings.desc.market_color':                   'Choose how rising/falling prices are colored (includes a colorblind-friendly palette).',
  'settings.option.market_color.red_up':          'Red Up / Green Down',
  'settings.option.market_color.green_up':        'Green Up / Red Down',
  'settings.option.market_color.colorblind':      'Colorblind (Amber Up / Blue Down)',
  'settings.option.market_color.red_up.desc':     'Gains in red, losses in green (East-Asian market convention).',
  'settings.option.market_color.green_up.desc':   'Gains in green, losses in red (Western market convention).',
  'settings.option.market_color.colorblind.desc': 'Amber and blue distinguish gains/losses; friendly to red-green color blindness.',

  // ── Dialogue — choice panel ─────────────────────────────────
  'dialogue.emphasis.danger':     'Danger',
  'dialogue.emphasis.one_chance': 'One Chance',
  'dialogue.emphasis.retry':      'Retryable',
  'dialogue.emphasis.intimacy':   'Intimacy',
  'dialogue.emphasis.advantage':  'Advantage',
  'dialogue.emphasis.trade':      'Trade',
  'dialogue.emphasis.offense':    'Offense',
  'dialogue.emphasis.important':  'Important',
  'dialogue.hint.danger':     'This choice has irreversible consequences.',
  'dialogue.hint.one_chance': 'Involves a check — you only get one attempt.',
  'dialogue.hint.retry':      'Involves a check that can be retried.',
  'dialogue.hint.intimacy':   'Deepens your relationship with this character.',
  'dialogue.hint.advantage':  'You hold an advantage in this exchange.',
  'dialogue.hint.trade':      'Opens a trade panel.',
  'dialogue.hint.offense':    'This topic may damage your relationship with them.',
  'dialogue.hint.important':  'Tied to a tracked quest or the main story.',
  'dialogue.choice.locked':   'Requirements not met',
};

export default en;
