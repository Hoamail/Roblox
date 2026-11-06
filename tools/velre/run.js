'use strict';
// Elevated Frida runner: attach to an elevated target and stream the agent's
// messages to a log file (one JSON record per line).
//   node run.js <pid> <seconds> <outfile>
const fs = require('fs');
const path = require('path');

const FRIDA = 'C:/Users/adminn/AppData/Roaming/npm/node_modules/rea-agents/node_modules/frida';
const frida = require(FRIDA);

const pid = parseInt(process.argv[2] || '0', 10);
const secs = parseFloat(process.argv[3] || '10');
const out = process.argv[4] || path.join(__dirname, 'vel_frida.log');
const agentSrc = fs.readFileSync(path.join(__dirname, 'agent.js'), 'utf8');

function log(obj) {
  fs.appendFileSync(out, JSON.stringify(obj) + '\n');
}

(async () => {
  log({ t: 'runner', pid, secs, node: process.version });
  const session = await frida.attach(pid);
  log({ t: 'attached' });
  const script = await session.createScript(agentSrc, { name: 'velre' });
  script.message.connect((msg) => {
    if (msg.type === 'send') log(msg.payload);
    else log({ t: 'error', payload: msg.payload, description: msg.description });
  });
  await script.load();
  log({ t: 'loaded' });
  setTimeout(async () => {
    try { await script.unload(); } catch (e) {}
    try { await session.detach(); } catch (e) {}
    log({ t: 'done' });
    process.exit(0);
  }, secs * 1000);
})().catch((e) => {
  log({ t: 'fatal', error: String(e), stack: e && e.stack });
  process.exit(1);
});
