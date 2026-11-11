// What a page's style sheet asks for that the engine does not take (docs/devtools.md), from a running dev build.
//
//   node tools/devtools_report.mjs <host:port>
//
// Prints each declaration with its line and the elements it applies to, what else the sheet's parser could not take, and
// what the page's cascades dropped. Exits 1 when there is anything to print, so that a build can fail on it.
const [address] = process.argv.slice(2);
if (!address) {
  console.error('usage: devtools_report.mjs <host:port>');
  process.exit(2);
}
const targets = await (await fetch(`http://${address}/json/list`)).json();
if (!targets.length) {
  console.error('the page lists no targets');
  process.exit(2);
}
let found = 0;
for (const target of targets) {
  const socket = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve);
    socket.addEventListener('error', reject);
  });
  const report = await new Promise((resolve) => {
    socket.addEventListener('message', (event) => {
      const message = JSON.parse(event.data);
      if (message.id === 1) {
        resolve(message.result);
      }
    });
    socket.send(JSON.stringify({id: 1, method: 'Weft.getCompatibilityReport'}));
  });
  socket.close();
  console.log(report.sheet);
  for (const d of report.declarations) {
    console.log(`  ${d.line}: ${d.selector} { ${d.property}: ${d.value} } (${d.elements} elements): ${d.reason}`);
  }
  for (const p of report.problems) {
    console.log(`  ${p.line}: ${p.message}`);
  }
  for (const line of report.runtime) {
    console.log(`  ${line}`);
  }
  found += report.declarations.length + report.problems.length + report.runtime.length;
}
process.exit(found ? 1 : 0);
