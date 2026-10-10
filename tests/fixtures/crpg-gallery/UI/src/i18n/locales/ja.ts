/**
 * Japanese locale — must satisfy the same key set as zh-CN.
 */
import type { LocaleKeys } from './zh-CN';

const ja: Record<LocaleKeys, string> = {
  // ── Main Menu ──────────────────────────────────────────────
  'menu.badge':         'CRPG Framework',
  'menu.title.line1':   'Purple',
  'menu.title.line2':   'Scheme',
  'menu.tagline':       '陰謀と野望、運命の影が織りなす伝説の物語。',

  'menu.start_game':          'はじめる',
  'menu.start_game.subtitle': '旅立ちの時',
  'menu.continue':            'つづきから',
  'menu.continue.subtitle':   '最新のセーブをロード',
  'menu.settings':            '設定',
  'menu.settings.subtitle':   'オプションを調整',
  'menu.exit':                '終了',
  'menu.exit.subtitle':       'ゲームを終わる',

  'menu.footer.build':      'ビルド',
  'menu.footer.engine':     'エンジン',
  'menu.footer.framework':  'フレームワーク',

  // ── HUD ────────────────────────────────────────────────────
  'hud.not_connected': 'UE に接続されていません',

  // ── Settings — shell ───────────────────────────────────────
  'settings.title':              '設定',
  'settings.apply':              '適用',
  'settings.cancel':             'キャンセル',
  'settings.waiting_data':       '設定データを待機中…',
  'settings.detail_placeholder': '項目にカーソルを合わせると詳細が表示されます',

  // ── Settings — categories ──────────────────────────────────
  'settings.category.interface':     'インターフェース',
  'settings.category.audio':         'サウンド',
  'settings.category.video':         'グラフィック',
  'settings.category.input':         '入力',
  'settings.category.accessibility': 'アクセシビリティ',

  // ── Settings — Interface (client-only) ─────────────────────
  'settings.label.language':        '言語',
  'settings.desc.language':         'インターフェースの表示言語を切り替えます。',
  'settings.label.ui_font':         'UI フォント',
  'settings.desc.ui_font':          'メニューや本文に使われるフォント。',
  'settings.label.display_font':    '見出しフォント',
  'settings.desc.display_font':     'タイトルや強調表示に使われるフォント。',
  'settings.label.subtitle_font':   '字幕フォント',
  'settings.desc.subtitle_font':    '字幕の表示フォント。混合書体ではアルファベットは Garamond、数字は Cinzel Decorative を使用します。',

  // ── Settings — Subtitle (client-only, injected into accessibility) ─
  'settings.subtitle.header':             '字幕',
  'settings.label.show_speaker_name':     '話者名を表示',
  'settings.desc.show_speaker_name':      '字幕の前に話者の名前を表示します（例: 「アルドリック: こんにちは」）。',
  'settings.label.show_speaker_color':    '話者色をハイライト',
  'settings.desc.show_speaker_color':     '話者名をキャラクターカラーで色付けします。',
  'settings.label.subtitle_font_scale':   '字幕サイズ',
  'settings.desc.subtitle_font_scale':    '字幕の文字サイズ倍率（1.0 = 既定）。',
  'settings.label.subtitle_bg_opacity':   '字幕背景',
  'settings.desc.subtitle_bg_opacity':    '字幕の背景の濃さ（0 = 透明、1 = 不透明）。',

  // Furigana density — Japanese-only setting
  'settings.label.furigana_mode':         'ふりがな',
  'settings.desc.furigana_mode':          '字幕の漢字に振るふりがなの表示量を選びます。',
  'settings.option.furigana.off':         '漢字のみ',
  'settings.option.furigana.key':         '重要語のみ',
  'settings.option.furigana.all':         '全てに表示',
  'settings.option.furigana.off.desc':    'ふりがなを表示しません。例：「魔法で異世界に来た。」',
  'settings.option.furigana.key.desc':    '作者が指定した重要語のみ。例：「[魔法]!{まほう}で異世界に来た。」',
  'settings.option.furigana.all.desc':    '台本中の全てのふりがな。例：「[魔法]!{まほう}で[異世界]{い|せ|かい}に来た。」',

  // ── Settings — Accessibility (extensions) ──────────────────
  'settings.label.ui_scale':        'UI スケール',
  'settings.desc.ui_scale':         'すべての UI 要素を比例的に拡大縮小します。',

  // ── Settings — Input / Controls ────────────────────────────
  'settings.label.sprint_mode':         'ダッシュ操作',
  'settings.desc.sprint_mode':          'ダッシュキーを押し続けるか、一度押しで切り替えるかを選びます。',
  'settings.option.sprint_mode.hold':   '押し続け',
  'settings.option.sprint_mode.toggle': '切り替え',
  'settings.option.sprint_mode.hold.desc':   'キーを押している間だけダッシュ。離すと解除。',
  'settings.option.sprint_mode.toggle.desc': '一度押すとダッシュ開始、もう一度押すと解除。',

  // ── Settings — Market (client-only, injected into accessibility) ─
  'settings.market.header':                       '市場',
  'settings.label.market_color':                  '騰落の配色',
  'settings.desc.market_color':                   '株価の上昇・下落を表す色を選びます（色覚補助の配色を含む）。',
  'settings.option.market_color.red_up':          '赤上昇・緑下降',
  'settings.option.market_color.green_up':        '緑上昇・赤下降',
  'settings.option.market_color.colorblind':      '色覚補助（橙上昇・青下降）',
  'settings.option.market_color.red_up.desc':     '上昇は赤、下降は緑（東アジア市場の慣例）。',
  'settings.option.market_color.green_up.desc':   '上昇は緑、下降は赤（欧米市場の慣例）。',
  'settings.option.market_color.colorblind.desc': '橙と青で騰落を区別。赤緑色覚に配慮。',

  // ── Dialogue — choice panel ─────────────────────────────────
  'dialogue.emphasis.danger':     '危険',
  'dialogue.emphasis.one_chance': '一度きり',
  'dialogue.emphasis.retry':      '再挑戦可',
  'dialogue.emphasis.intimacy':   '親密',
  'dialogue.emphasis.advantage':  '優位',
  'dialogue.emphasis.trade':      '取引',
  'dialogue.emphasis.offense':    '無礼',
  'dialogue.emphasis.important':  '重要',
  'dialogue.hint.danger':     'この選択は取り返しのつかない結果を招きます。',
  'dialogue.hint.one_chance': '判定あり——チャンスは一度だけです。',
  'dialogue.hint.retry':      '判定あり——失敗しても再挑戦できます。',
  'dialogue.hint.intimacy':   '相手との親密度が深まります。',
  'dialogue.hint.advantage':  'この会話ではあなたが優位に立っています。',
  'dialogue.hint.trade':      '取引画面を開きます。',
  'dialogue.hint.offense':    'この話題は相手との関係を損なう恐れがあります。',
  'dialogue.hint.important':  '追跡中のクエストや本筋の物語に関わります。',
  'dialogue.choice.locked':   '条件を満たしていません',
};

export default ja;
