/**
 * 简体中文 — 母语，所有 key 以此为基准。
 */
const zhCN = {
  // ── Main Menu ──────────────────────────────────────────────
  "menu.badge": "CRPG Framework",
  "menu.title.line1": "Purple",
  "menu.title.line2": "Scheme",
  "menu.tagline": "一段关于阴谋、野心与命运暗影的传奇。",

  "menu.start_game": "开始游戏",
  "menu.start_game.subtitle": "踏上旅途",
  "menu.continue": "继续游戏",
  "menu.continue.subtitle": "加载最近存档",
  "menu.settings": "设置",
  "menu.settings.subtitle": "调整选项",
  "menu.exit": "退出",
  "menu.exit.subtitle": "离开游戏",

  "menu.footer.build": "版本",
  "menu.footer.engine": "引擎",
  "menu.footer.framework": "框架",

  // ── HUD ────────────────────────────────────────────────────
  "hud.not_connected": "未连接到 UE",

  // ── Settings — shell ───────────────────────────────────────
  "settings.title": "设置",
  "settings.apply": "应用",
  "settings.cancel": "取消",
  "settings.waiting_data": "正在等待设置数据…",
  "settings.detail_placeholder": "将鼠标悬停在设置项上查看详情",

  // ── Settings — categories ──────────────────────────────────
  "settings.category.interface": "界面",
  "settings.category.audio": "音频",
  "settings.category.video": "画面",
  "settings.category.input": "输入",
  "settings.category.accessibility": "辅助功能",

  // ── Settings — Interface (client-only) ─────────────────────
  "settings.label.language": "语言",
  "settings.desc.language": "切换界面文字显示语言。",
  "settings.label.ui_font": "界面字体",
  "settings.desc.ui_font": "用于菜单与正文文本的字体。",
  "settings.label.display_font": "标题字体",
  "settings.desc.display_font": "用于标题与特殊强调文本的字体。",
  "settings.label.subtitle_font": "字幕字体",
  "settings.desc.subtitle_font": "字幕文本字体。混合字体下，中文使用宋体，数字与拉丁字符使用 Cinzel Decorative。",

  // ── Settings — Subtitle (client-only, injected into accessibility) ─
  "settings.subtitle.header": "字幕设置",
  "settings.label.show_speaker_name": "显示说话者名字",
  "settings.desc.show_speaker_name": "显示说话者姓名",
  "settings.label.show_speaker_color": "说话者颜色高亮",
  "settings.desc.show_speaker_color": "说话者高亮姓名。",
  "settings.label.subtitle_font_scale": "字幕字号",
  "settings.desc.subtitle_font_scale": "调整字幕文字大小",
  "settings.label.subtitle_bg_opacity": "字幕背景",
  "settings.desc.subtitle_bg_opacity": "字幕背景不透明度",

  // Furigana density — Japanese-only setting (visibleWhen language === 'ja')
  "settings.label.furigana_mode": "假名注音",
  "settings.desc.furigana_mode": "决定字幕中汉字上方假名注音的显示密度。",
  "settings.option.furigana.off": "全汉字",
  "settings.option.furigana.key": "重要词注音",
  "settings.option.furigana.all": "全注音",
  "settings.option.furigana.off.desc": "隐藏全部注音，例：「魔法で異世界に来た。」",
  "settings.option.furigana.key.desc": "仅显示作者标记的关键词，例：「[魔法]!{まほう}で異世界に来た。」",
  "settings.option.furigana.all.desc": "显示剧本中全部注音，例：「[魔法]!{まほう}で[異世界]{い|せ|かい}に来た。」",

  // ── Settings — Accessibility (extensions) ──────────────────
  "settings.label.ui_scale": "界面缩放",
  "settings.desc.ui_scale": "等比缩放全部界面元素，数值越大字体与控件越大。",

  // ── Settings — Input / Controls ────────────────────────────
  "settings.label.sprint_mode": "冲刺方式",
  "settings.desc.sprint_mode": "决定冲刺键是按住生效还是单击切换。",
  "settings.option.sprint_mode.hold": "按住",
  "settings.option.sprint_mode.toggle": "切换",
  "settings.option.sprint_mode.hold.desc": "按住冲刺键时进入冲刺，松开恢复。",
  "settings.option.sprint_mode.toggle.desc": "单击冲刺键切换状态，再按一次取消。",

  // ── Settings — Market (client-only, injected into accessibility) ─
  "settings.market.header": "市场",
  "settings.label.market_color": "涨跌配色",
  "settings.desc.market_color": "选择股票涨跌的颜色约定（含红绿色盲友好方案）。",
  "settings.option.market_color.red_up": "红涨绿跌",
  "settings.option.market_color.green_up": "绿涨红跌",
  "settings.option.market_color.colorblind": "色盲友好（橙涨蓝跌）",
  "settings.option.market_color.red_up.desc": "上涨红色、下跌绿色（中国市场惯例）。",
  "settings.option.market_color.green_up.desc": "上涨绿色、下跌红色（欧美市场惯例）。",
  "settings.option.market_color.colorblind.desc": "用橙色与蓝色区分涨跌，对红绿色盲更友好。",

  // ── Dialogue — choice panel (强调类别标签 + 默认 hover 提示) ──
  "dialogue.emphasis.danger": "危险",
  "dialogue.emphasis.one_chance": "一次机会",
  "dialogue.emphasis.retry": "可反复尝试",
  "dialogue.emphasis.intimacy": "亲密",
  "dialogue.emphasis.advantage": "优势",
  "dialogue.emphasis.trade": "交易",
  "dialogue.emphasis.offense": "冒犯",
  "dialogue.emphasis.important": "重要",
  "dialogue.hint.danger": "该选项会产生不可逆的后果。",
  "dialogue.hint.one_chance": "包含一次检定，只有一次机会。",
  "dialogue.hint.retry": "包含一次检定，失败后可反复尝试。",
  "dialogue.hint.intimacy": "会推进你与对方的亲密关系。",
  "dialogue.hint.advantage": "你在这段对白中享有优势。",
  "dialogue.hint.trade": "将打开交易面板。",
  "dialogue.hint.offense": "该话题可能损害你与交谈者的关系。",
  "dialogue.hint.important": "与被追踪的任务或主线故事息息相关。",
  "dialogue.choice.locked": "条件不满足",
} as const;

export default zhCN;
export type LocaleKeys = keyof typeof zhCN;
