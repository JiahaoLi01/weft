# Web Animations（`element.animate()`）

2026-11-08 起（`weft-ingame` 分支），用户在缺口清单里选了这一项。本文记录第一阶段的范围、做法、与 Chrome 的对照证据、差别和尚未做的部分。

## 第一阶段

### 范围

- **入口**：`element.animate(keyframes, options)`、`element.getAnimations({subtree})`、`document.getAnimations()`、`document.timeline`。
- **对象**：
  - `new KeyframeEffect(target, keyframes, options)`（也可复制另一个效果）、`new Animation(effect, document.timeline)`；
  - `AnimationEffect`、`AnimationTimeline`、`DocumentTimeline`、`AnimationPlaybackEvent`。
- **关键帧**：
  - 数组写法与按属性写法；
  - `offset`（缺省的均匀分布，首尾取 0 和 1）、每帧的 `easing`；
  - 驼峰属性名（`backgroundColor`、`cssFloat`）；
  - 只有一端的关键帧，缺的一端取下面的值。
  - 能做动画的属性与 CSS 动画相同（场景的插值规则）。
- **计时**：`delay`、`endDelay`、`duration`（含 `auto`）、`iterations`（含 `Infinity`）、`iterationStart`、`direction` 四种、`fill`（含 `auto`）、`easing`（`linear`、`ease` 系列、`cubic-bezier()`、`steps()` 各种跳变位置、`step-start`、`step-end`）。非法值抛 TypeError。
- **控制**：
  - `play`、`pause`、`finish`、`cancel`、`reverse`、`updatePlaybackRate`、`persist`、`commitStyles`；
  - `currentTime`、`startTime`、`playbackRate` 可读写；
  - `playState`、`pending`、`replaceState`、`id`、`effect`（可换）、`timeline`。
- **Promise 与事件**：
  - `ready`、`finished`：重新开始时换新的；取消时以 AbortError 拒绝；
  - `finish`、`cancel`、`remove` 事件与 `onfinish`、`oncancel`、`onremove`。
- **`KeyframeEffect`**：`target`（可换）、`getKeyframes()`（含 `computedOffset`）、`setKeyframes()`、`getTiming()`、`getComputedTiming()`、`updateTiming()`。
- **合成顺序**：脚本动画在 CSS 动画和过渡之上，按创建顺序叠加，后来的覆盖前面的（`replace`）。
- **自动移除**：已完成、仍在生效、每个属性都被更新的已完成动画覆盖的动画，标为 `removed`，并发出 `remove` 事件；`persist()` 的不移除。

### 做法

- **计时模型在 JS**（`src/script/js/scene_platform_bootstrap.js` 末尾一段）：
  - 按规范的过程实现：播放、暂停、完成、取消、反向、设定播放速率、设定当前与开始时间、更新完成状态、待定任务、移除被替换的动画。
  - 时间线就是 View 的时钟（帧的时间）。
- **每帧的更新**（`animation_frame_bootstrap.js` 里帧回调宿主的 `begin`）：
  - 时间线取这一帧的时间；
  - 完成待定的播放与暂停（开始时间定在这一帧）；
  - 更新完成状态；
  - 移除被替换的动画。
- **事件**：排队，在本帧回调之前派发，排在这一步产生的微任务之后。因此 `finished` 的 Promise 先于 `finish` 事件，取消时的 AbortError 先于 `cancel` 事件，与 Chrome 相同。
- **效果的值由场景求**：
  - 动画状态一变，就经 `__ceAnimation` 把关键帧（转成 CSS 声明）、计时和开始或保持时间、速率交给目标元素的 DOM 节点（`src/dom/animation.hpp`，随快照传给场景）。
  - 场景求解器（`scene.cpp` 的 `script_progress`）每帧按规范 §4 算出进度，在 CSS 动画之后按关键帧插值，与 CSS 动画共用插值和类型化的绘制值。
  - 播放中的动画让场景每帧重算，与 CSS 动画相同；JS 不必每帧给出数值。
  - 动画完成后保持时间固定，场景不再每帧重算。不在生效中的已完成动画会从每帧的列表里拿掉。
- **原生缓动**：补上 `step-start` 和 `steps()` 的 `start`、`jump-start`、`jump-none`、`jump-both`，CSS 动画也能用了。
- **`getComputedStyle`**：插值得到的 `rgb()`/`rgba()` 颜色按 Chrome 的方式输出，即通道取整、不透明时写 `rgb(r, g, b)`。作者直接写的颜色保持原样，`#ff0000` 仍是 `#ff0000`。
- 只有动画变化的节点，求解器也会重建（`same_dom_own` 比较动画）。

### 证据

2026-11-08，macOS 26.6，Apple M5，Debug，Chrome 156（无头）。

- **夹具与工具**：`tests/fixtures/web_animations/animations.js` 的 8 组步骤，由 `tools/browser_web_animations_reference.mjs` 在 Chrome 里跑，记录 91 条日志。
  - 8 组步骤：基本、关键帧写法、计时（14 种组合）、播放控制、事件与 Promise、叠加、替换、构造。
  - 只记与真实帧时间无关的量：暂停后设定当前时间时的计算值，经过的状态，事件与 Promise 的先后。
- **`web_animations_browser_contract`**（可移植）：
  - 同一脚本在 View 里每帧 16 ms 推进，91 条日志与 Chrome 逐条一致。
  - 另加一段播放中的检查：每一帧的 `getComputedStyle` 值都等于当前时间推出的进度，结束后回到元素自己的值。这一段按规范计算，不是录制的。
- **变异检查**：把脚本动画的合成顺序反过来，4 条日志不一致。
- **全量 `ctest -j8`**（`build/macos-img2`）：87/95，失败的仍是原来那 8 项 fixture 检查。
- **iPad 真机**（2026-11-08，iPad Pro 13 英寸 M4，iPadOS 26.6.2，Release）：
  - `examples/animate` 先在真实的 120 Hz 帧上跑同一组步骤，再与 Chrome 的日志比较：91/91 一致。
    - 日志的副本是 `examples/animate/chrome-log.js`；它与 `chrome-results.json` 不一致时，`web_animations_browser_contract` 报错。
    - 步骤脚本按 page.json 里的相对路径，从测试夹具读取；iOS 包里放在相同的相对位置。
  - 之后页面上播放的几个动画都是 120 fps，第一秒以后没有掉帧。
  - 页面上的点按控制（播放、暂停、反向、调速、替换与 `commitStyles`）没有在真机上试过。

### 与 Chrome 的差别

- 待定的播放在下一帧开始。Chrome 在下一帧提交时开始，两者都是“下一帧”，具体时刻可能相差一帧之内。
- `steps()` 在阶段边界上的“之前”标记没有实现。只影响 `jump-start` 类缓动在延迟结束那一刻的取值。

### 尚未实现与未验证

- `composite: 'add'` 和 `'accumulate'`（抛 NotSupportedError）、`iterationComposite`。
- 只支持文档时间线：`ScrollTimeline`、`ViewTimeline` 和 `timeline: null` 都没有。
- 伪元素目标。
- `getAnimations()` 不返回 CSS 动画与过渡，没有 `CSSAnimation`、`CSSTransition` 对象。
- `commitStyles()` 不检查元素是否渲染。
- 每次状态变化都把整组关键帧交给原生，元素的关键帧随之重新解析。播放中无此开销。
- **未验证**：Windows、Android 没有运行；iOS 只在 iPad 上跑过上面那一页。这里的改动不涉及渲染后端，只在场景和脚本里。
