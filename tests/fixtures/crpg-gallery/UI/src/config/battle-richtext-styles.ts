/**
 * Battle RichText Styles —— 战斗 UI 富文本的 inline 样式表。
 *
 * 配合 `<RichText styles={BATTLE_RICHTEXT_STYLES} ...>` 解析 `{style.<name>:<text>}` token,
 * 把每个 token 包成一个 `<span class="...">{text}</span>`,class 来自这张表的 value。
 *
 * 设计动机:**维多利亚商品 / 武器铭牌混合字体审美**。19 世纪商品包装(Coca-Cola 招牌、
 * 古董酒标、Wild West "Wanted" 海报)惯用一行内多字体混排 —— 品牌名 Cinzel 大写、口径
 * 用细花体小数字、修饰词走斜体衬线。框架内的步枪 / 法器 / 卷轴等装备命名要走这个风格,
 * 不能用单字体平铺。
 *
 * 使用示例(放在 ability / variant displayName 里):
 *   "{style.brand:V&C} {style.display:Cathedral} {style.flourish:Repeater} {style.caliber:.44}"
 * 渲染效果:
 *   - V&C        →  粗体小一号无衬线(maker 名,扎实质感)
 *   - Cathedral  →  Cinzel Decorative 大写,加宽字距,放大字号(主商品名)
 *   - Repeater   →  斜体衬线,加点字距(修饰词 / 类别)
 *   - .44        →  Cinzel Decorative 数字,italic(口径 / 序号)
 *
 * --------------------------------------------------------------------------------
 * 关于 `--font-subtitle`:
 *   项目 theme.css 定义的五段混合 stack(Cinzel Decorative 数字 + EB Garamond 拉丁 +
 *   Shippori Mincho 日文 kana + LXGW NeoZhiSong Plus 汉字 + Noto Serif SC 兜底)。
 *   `unicode-range` scope 让每段字体只覆盖各自字符,**单个 className 就能渲染中英日数字混排**,
 *   不用 caller 知道字符种类。下面的 victorian 样式全部走这条 stack 当 base font,
 *   靠 letter-spacing / font-weight / font-size / font-style 拉差异。
 *
 * --------------------------------------------------------------------------------
 * 添加新样式:
 *   1. 在 BATTLE_RICHTEXT_STYLES 加一条 `<name>: <className>`
 *   2. 设计师在 ability / variant displayName 里写 `{style.<name>:<text>}` 引用
 *   3. 不需要重启 / build —— Tailwind v4 JIT 直接生成对应 class
 *
 * 命名建议:**语义化而非视觉化** —— `brand` / `display` / `caliber` 描述这条用在哪种位置,
 * 比 `bold-italic-large` 描述长啥样更耐改(策划调整某个语义的视觉时只动 className,引用处不动)。
 */

export const BATTLE_RICHTEXT_STYLES: Record<string, string> = {
  // ─── 维多利亚商品 / 武器铭牌混合字体 ────────────────────────────
  //
  // 这一组想象成"在金属铭牌 / 木盒商标 / 蜡封说明书上面看到的字体"。
  // 每条都基于 --font-subtitle 走自动 unicode-range 切字体,只调权重 / 字距 / 字号。

  /**
   * **品牌 / 制造商名**(典型:V&C / Winchester / Black Powder Co.)。
   * 粗体 + 紧字距 + 略小字号,跟主商品名拉差异,有"压印感"。
   */
  brand:
    "font-[family-name:var(--font-handWrite-trademark-upscale)] font-bold tracking-tight text-[1.05em]",

  /**
   * **主商品名 / 大字招牌**(典型:Cathedral / Pestilence / Cordial)。
   * Cinzel Decorative 全大写,拉宽字距,加大字号 —— 维多利亚招牌的"标语"位。
   * 配合 brand 一起用形成"V&C 'Cathedral'"这种主副结构。
   */
  display:
    "font-[family-name:var(--font-subtitle)] uppercase tracking-[0.18em] text-[1.1em]",

  /**
   * **修饰词 / 子类别**(典型:Repeater / Revolver / Pepperbox / Carbine)。
   * 斜体衬线 + 轻字距,像古书排版里的 italic adjective,有文雅的修饰感。
   */
  flourish: "font-[family-name:var(--font-subtitle)] italic tracking-[0.06em]",

  /**
   * **口径 / 序号 / 型号数字**(典型:".44" / "No. II" / "Mk.III")。
   * Cinzel Decorative 走数字字符,italic 给"手写记号"质感。
   * 跟 display 视觉上对比强(一个端正大写,一个倾斜小数字),组合不撞。
   */
  caliber:
    "font-[family-name:var(--font-subtitle)] italic text-[0.95em] tracking-wide",

  /**
   * **小标 / 副标题**(典型:"Bespoke Edition" / "Marksman's Pride")。
   * 全小写 small-caps + 拉字距 + 缩小字号,像古书脚注或商品标签里的"副刊"位。
   */
  "small-caps":
    "font-[family-name:var(--font-subtitle)] uppercase text-[0.78em] tracking-[0.22em]",

  /**
   * **金色强调**(典型:legendary / unique 等级的物品名,或炫光感的关键词)。
   * 颜色压在金色调上,小阴影模拟"烫金"。其它字体属性维持 inherit 让它能跟 brand /
   * display 等叠加使用("brand 套金"不冲突)。
   */
  gold: "text-gold-light",

  /**
   * **古朴 / 旧化感**(典型:Antique / Worn / Salvaged 等磨损 lore 词)。
   * parchment-dark 字色 + 斜体 + 轻字距,印染感更弱,代表"老"。
   */
  worn: "italic text-parchment-dark tracking-wide",

  // ─── 通用 ────────────────────────────────────────────────────
  // 文学性词的内联强调(描述里的"必胜"、"致命"这种重音词)。

  /** 内联强调(粗体 + 微亮)—— 描述句中关键词加重。 */
  emphasis: "font-semibold text-parchment",

  /** 数字 / 数值高亮 —— 伤害公式里的 `6d6` / `+50%` 等数字段,Cinzel 雕花数字。 */
  number:
    "font-[family-name:var(--font-subtitle)] tabular-nums text-gold-light",
};
