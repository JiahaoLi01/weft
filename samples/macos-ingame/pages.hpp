#pragma once

// The pages on the in-game sample's panels: plain DOM and strict-profile CSS, as a game's UI would be written. Each
// talks to the game through weft.engine: the terminal sends reactor.output when a button changes the reactor's output;
// the status board shows the game's status event, sent once a second; the sign is static.
namespace ingame {
    struct PageSource {
        const char* name;
        const char* script;
        const char* stylesheet;
        const char* keyframes;
    };

    // A control terminal, 480 x 320 px: the reactor's output in large digits and a bar, three buttons that change it
    // (the game's core follows), and a log of the last three changes. Its panel is inset 14 px for its glow; its
    // buttons stand at fixed places, so the game's scripted click can aim at one.
    inline constexpr PageSource terminal{
        "terminal",
        R"JS((() => {
  const engine = weft.engine;
  const root = document.getElementById('root');
  const el = (parent, cls, text) => {
    const e = document.createElement('div');
    e.className = cls;
    if (text !== undefined) e.textContent = text;
    parent.appendChild(e);
    return e;
  };
  const panel = el(root, 'panel');
  el(panel, 'title', 'REACTOR CONTROL');
  const state = el(panel, 'state', 'STABLE');
  const value = el(panel, 'value', '40');
  el(panel, 'unit', '% OUTPUT');
  const bar = el(panel, 'bar');
  const fill = el(bar, 'fill');
  const row = el(panel, 'buttons');
  const log = el(panel, 'log');
  const lines = [el(log, 'line', 'Reactor online'), el(log, 'line', ''), el(log, 'line', '')];
  const said = ['Reactor online'];
  let output = 40;
  const say = (text) => {
    said.unshift(text);
    said.length = Math.min(said.length, lines.length);
    lines.forEach((line, i) => { line.textContent = said[i] || ''; });
  };
  const show = () => {
    value.textContent = String(output);
    fill.style.width = output + '%';
    const level = output >= 90 ? 'critical' : output >= 70 ? 'high' : 'stable';
    state.textContent = level.toUpperCase();
    state.className = 'state ' + level;
    engine.send('reactor.output', output);
  };
  const button = (label, cls, act) => {
    const b = el(row, cls ? 'button ' + cls : 'button', label);
    b.addEventListener('click', act);
  };
  button('-10', '', () => { output = Math.max(0, output - 10); say('Output lowered to ' + output + '%'); show(); });
  button('+10', '', () => { output = Math.min(100, output + 10); say('Output raised to ' + output + '%'); show(); });
  button('VENT', 'warn', () => { output = 10; say('Coolant vented'); show(); });
  show();
})();)JS",
        "#root{position:relative;width:100%;height:100%;font-family:sans-serif;color:#d6f1ff}"
        ".panel{position:absolute;left:14px;top:14px;right:14px;bottom:14px;background-color:rgba(4,14,24,0.82);"
        "border:2px solid #38b6e8;border-radius:14px;box-shadow:0 0 14px rgba(56,182,232,0.7)}"
        ".title{position:absolute;left:20px;top:14px;font-size:15px;line-height:20px;color:#7fd6f7}"
        ".state{position:absolute;right:20px;top:12px;width:96px;height:24px;font-size:13px;line-height:24px;"
        "text-align:center;border-radius:12px;color:#06141e;background-color:#4fd18b}"
        ".high{background-color:#f2b23a}"
        ".critical{background-color:#ff5a4f}"
        ".value{position:absolute;left:20px;top:40px;width:120px;font-size:64px;line-height:76px;color:#ffffff}"
        ".unit{position:absolute;left:146px;top:76px;font-size:15px;line-height:20px;color:#7fd6f7}"
        ".bar{position:absolute;left:20px;right:20px;top:126px;height:14px;border-radius:7px;"
        "background-color:rgba(127,214,247,0.15)}"
        ".fill{width:40%;height:100%;border-radius:7px;background-image:linear-gradient(to right,#38b6e8,#ffd166);"
        "transition:width 0.25s ease-out}"
        ".buttons{position:absolute;left:20px;top:158px;display:flex;gap:12px}"
        ".button{width:120px;height:44px;font-size:18px;line-height:40px;text-align:center;border:2px solid #38b6e8;"
        "border-radius:8px;background-color:rgba(56,182,232,0.12);cursor:pointer}"
        ".button:hover{background-color:rgba(56,182,232,0.38)}"
        ".button:active{background-color:rgba(56,182,232,0.65)}"
        ".warn{border-color:#ff5a4f;background-color:rgba(255,90,79,0.12)}"
        ".warn:hover{background-color:rgba(255,90,79,0.38)}"
        ".log{position:absolute;left:20px;right:20px;top:214px}"
        ".line{height:18px;font-size:13px;line-height:18px;color:#8fb3c4}",
        nullptr};

    // The terminal's logical size and where its +10 button's center is (the panel inset 14 px, its 2 px border, the
    // row at 20 px and 158 px, buttons 120 x 44 px 12 px apart).
    inline constexpr float terminal_width = 480;
    inline constexpr float terminal_height = 320;
    inline constexpr float plus_ten_x = 14 + 2 + 20 + 120 + 12 + 60;
    inline constexpr float plus_ten_y = 14 + 2 + 158 + 22;

    // A status board, 420 x 260 px: the game's clock, the core's temperature over the last sixteen seconds as
    // columns, and a line of its state; a scanning bar that animates every frame when the game asks (status.animate),
    // to measure a page that changes all the time.
    inline constexpr PageSource status{
        "status",
        R"JS((() => {
  const engine = weft.engine;
  const root = document.getElementById('root');
  const el = (parent, cls, text) => {
    const e = document.createElement('div');
    e.className = cls;
    if (text !== undefined) e.textContent = text;
    parent.appendChild(e);
    return e;
  };
  const board = el(root, 'board');
  el(board, 'head', 'SECTOR 7 - CORE TEMPERATURE');
  const clock = el(board, 'clock', '00:00:00');
  const chart = el(board, 'chart');
  const cols = [];
  for (let i = 0; i < 16; i++) cols.push(el(chart, 'col'));
  const scan = el(board, 'scan');
  const foot = el(board, 'foot', 'Waiting for telemetry');
  engine.on('status', (s) => {
    clock.textContent = s.time;
    s.temps.forEach((v, i) => { cols[i].style.height = v + '%'; });
    foot.textContent = 'Output ' + s.output + '%  -  core ' + s.core + '  -  ' + s.temp + ' K';
  });
  engine.on('status.animate', (on) => { scan.className = on ? 'scan on' : 'scan'; });
})();)JS",
        "#root{position:relative;width:100%;height:100%;font-family:sans-serif;color:#e8f6ff}"
        ".board{position:absolute;left:10px;top:10px;right:10px;bottom:10px;background-color:rgba(10,12,20,0.9);"
        "border:2px solid #ffd166;border-radius:12px}"
        ".head{position:absolute;left:18px;top:14px;font-size:13px;line-height:18px;color:#ffd166}"
        ".clock{position:absolute;right:18px;top:10px;font-size:22px;line-height:26px;color:#ffffff}"
        ".chart{position:absolute;left:18px;right:18px;top:48px;height:120px;display:flex;align-items:flex-end;"
        "gap:6px}"
        ".col{flex-grow:1;height:30%;border-radius:3px;background-image:linear-gradient(to top,#ff8a3d,#ffd166)}"
        ".scan{position:absolute;left:18px;top:174px;width:60px;height:3px;background-color:#ffd166;opacity:0}"
        ".on{opacity:1;animation:scan 1.2s linear infinite}"
        ".foot{position:absolute;left:18px;right:18px;top:188px;font-size:14px;line-height:20px;color:#a9b8c8}",
        "@keyframes scan{from{transform:translateX(0)}to{transform:translateX(304px)}}"};

    // A sign, 1024 x 256 px, far off: large type and fine diagonal stripes, which shimmer without mips.
    inline constexpr PageSource sign{
        "sign",
        R"JS((() => {
  const root = document.getElementById('root');
  const el = (parent, cls, text) => {
    const e = document.createElement('div');
    e.className = cls;
    if (text !== undefined) e.textContent = text;
    parent.appendChild(e);
    return e;
  };
  const sign = el(root, 'sign');
  el(sign, 'big', 'WEFT');
  el(sign, 'sub', 'In-game UI  -  HTML and CSS on a texture');
  el(sign, 'stripes');
})();)JS",
        "#root{position:relative;width:100%;height:100%;font-family:sans-serif}"
        ".sign{position:absolute;left:0;top:0;right:0;bottom:0;border:4px solid #9fe3ff;border-radius:24px;"
        "background-image:linear-gradient(to right,#0b3a5a,#5a1f6e)}"
        ".big{position:absolute;left:40px;top:20px;font-size:132px;line-height:150px;color:#ffffff}"
        ".sub{position:absolute;left:44px;top:182px;font-size:30px;line-height:36px;color:#9fe3ff}"
        ".stripes{position:absolute;right:36px;top:36px;width:300px;height:176px;opacity:0.55;"
        "background-image:repeating-linear-gradient(45deg,#ffffff 0px,#ffffff 2px,rgba(255,255,255,0) 2px,"
        "rgba(255,255,255,0) 8px)}",
        nullptr};
} // namespace ingame
