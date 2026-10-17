// Chrome oracle for scene transitions: the transition shorthand and how the transition lists pair with
// transition-property (CSS Transitions 1 §2, §3). Each case is a stylesheet whose .t element changes opacity, transform
// and background-color when it gains the class "on"; Chrome's transitions are paused and seeked to each sample time (ms
// after the change) and the three values recorded, with each transition's timing and the computed longhands of the
// after-change style. Writes tests/fixtures/scene_transition_reference.json, which weft_scene_transition_tests runs
// through the scene. Offline: the page is a local file with the cases inline.
//
//   node tools/browser_scene_transition_reference.mjs [chrome.exe]
import {spawnSync} from 'node:child_process';
import {writeFileSync, mkdtempSync, existsSync, readdirSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve, join, dirname} from 'node:path';
import {pathToFileURL} from 'node:url';
import {repo} from './gallery_runtime.mjs';

const executable = process.argv[2] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if (!existsSync(executable)) throw new Error('Provide a local Chromium executable as the first argument');

// The before and after values of the three properties; every case specifies them in both states, so each change is a
// change between two specified values.
const base = '.t{width:40px;height:20px;opacity:0;transform:translateX(0px);background-color:#204080}' +
  '.t.on{opacity:1;transform:translateX(100px);background-color:#e0c010}';
const cases = [
  ['one', '.t{transition:opacity 400ms linear}'],
  // The Unity demo's button: two transitions, the default timing function.
  ['unity-list', '.t{transition:background-color 0.15s, transform 0.1s}'],
  // The first time is the duration, the second the delay, in any order with the rest; a negative delay.
  ['order-delay', '.t{transition:linear 300ms opacity 100ms, 200ms ease-in transform -100ms}'],
  // Shorter lists repeat to the length of transition-property.
  ['repeat-lists', '.t{transition-property:opacity, transform, background-color;transition-duration:400ms, 200ms;' +
    'transition-delay:0s, 100ms;transition-timing-function:linear}'],
  // Longer lists are cut to its length.
  ['excess-lists', '.t{transition-property:opacity;transition-duration:300ms, 100ms;transition-delay:50ms, 0s, 10ms;' +
    'transition-timing-function:linear, ease-in}'],
  // A later item that names a property wins over an earlier all, and the last of two items for it wins.
  ['all-then-override', '.t{transition:all 400ms linear, opacity 100ms ease-out}'],
  ['last-wins', '.t{transition:opacity 100ms linear, transform 300ms linear, opacity 300ms ease-in}'],
  // Timing functions with commas inside them.
  ['comma-functions', '.t{transition-property:opacity, transform;transition-duration:400ms;' +
    'transition-timing-function:cubic-bezier(0.1, 0.7, 1, 0.1), steps(4, end)}'],
  // var() in the shorthand: each longhand takes its list once the value is substituted.
  ['var', '.t{--speed:250ms;transition:opacity var(--speed) linear, transform var(--speed) linear 50ms, ' +
    'background-color var(--speed) ease-out}'],
  // A var() that substitutes no transition list: the longhands are invalid at computed value time (no transitions).
  ['var-invalid', '.t{--bad:1s 2s 3s;transition:opacity var(--bad)}'],
  ['none', '.t{transition:none}'],
  // A time alone transitions all properties.
  ['time-only', '.t{transition:300ms linear}'],
  // 0s with a delay: the start value through the delay, then the end value.
  ['zero-duration-delay', '.t{transition:opacity 0s 200ms, transform 200ms linear}'],
  // The after-change style's lists start the transitions.
  ['after-change', '.t{transition:opacity 400ms linear}.t.on{transition:opacity 100ms linear, transform 200ms linear}'],
  ['case-insensitive', '.t{transition:Opacity 200MS LINEAR, TRANSFORM 0.1S Ease-In}'],
].map(([name, sheet]) => ({name, sheet: base + sheet}));
const times = [0, 30, 90, 160, 230, 310, 390, 470, 620];
const properties = ['opacity', 'transform', 'background-color'];
const longhands = ['transition-property', 'transition-duration', 'transition-timing-function', 'transition-delay'];

const probe = `(() => {
  const style = document.createElement('style');
  document.head.appendChild(style);
  const read = (e, names) => Object.fromEntries(names.map((n) => [n, getComputedStyle(e).getPropertyValue(n)]));
  return ${JSON.stringify(cases)}.map(({name, sheet}) => {
    style.textContent = sheet;
    const e = document.createElement('div');
    e.className = 't';
    document.body.appendChild(e);
    getComputedStyle(e).opacity;
    e.className = 't on';
    const after = read(e, ${JSON.stringify(longhands)});
    const animations = e.getAnimations();
    for (const a of animations) a.pause();
    const transitions = animations.map((a) => {
      const t = a.effect.getTiming();
      return {property: a.transitionProperty, duration: t.duration, delay: t.delay, easing: t.easing};
    }).sort((a, b) => a.property < b.property ? -1 : 1);
    const samples = ${JSON.stringify(times)}.map((time) => {
      for (const a of animations) a.currentTime = time;
      return {time, values: read(e, ${JSON.stringify(properties)})};
    });
    for (const a of animations) a.cancel();
    e.remove();
    return {name, sheet, after, transitions, samples};
  });
})()`;
const page = '<!doctype html><html><head><meta charset=utf-8></head><body><pre id=report></pre><script>' +
  'document.getElementById("report").textContent = JSON.stringify({browser: navigator.userAgent, cases: ' +
  probe + '});</script></body></html>';
const folder = mkdtempSync(join(tmpdir(), 'weft-transition-'));
const file = join(folder, 'page.html');
writeFileSync(file, page);
const run = spawnSync(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--user-data-dir=' + join(folder, 'profile'), '--dump-dom', pathToFileURL(file).href],
  {encoding: 'utf8', maxBuffer: 64 * 1024 * 1024, windowsHide: true, timeout: 60000});
if (run.status !== 0) throw new Error('Chromium failed: ' + run.stderr);
const found = run.stdout.match(/<pre id="report">([\s\S]*?)<\/pre>/);
if (!found || !found[1]) throw new Error('no report in the page: ' + run.stdout.slice(0, 400));
const unescape = (s) => s.replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&amp;/g, '&');
const report = JSON.parse(unescape(found[1]));
// The user agent says only the major version; the install folder beside the executable is the full one.
const installed = readdirSync(dirname(executable)).find((name) => /^\d+\.\d+\.\d+\.\d+$/.test(name));
const chrome = installed ? 'Chrome/' + installed : (report.browser.match(/Chrome\/[\d.]+/) ?? [report.browser])[0];
writeFileSync(resolve(repo, 'tests/fixtures/scene_transition_reference.json'), JSON.stringify({
  format: 'weft-scene-transition-reference-v1', browser: chrome, properties, cases: report.cases}, null, 1) + '\n');
console.log(`Recorded ${report.cases.length} transition cases from ${chrome}`);
