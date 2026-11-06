'use strict';
// Frida agent: observe velext's writes/reads on Roblox memory.
// Read-only observation: no target memory is modified; the only code touched is
// inside velext (inline hooks of its own syscall stubs), which is what we are
// reverse engineering.

const MAINRE = /velext/i;
const main = Process.enumerateModules().find(m => MAINRE.test(m.name)) ||
             Process.enumerateModules()[0];
const mods = Process.enumerateModules();

function modOf(addr) {
  for (const m of mods) {
    if (addr.compare(m.base) >= 0 && addr.compare(m.base.add(m.size)) < 0)
      return { name: m.name, off: addr.sub(m.base) };
  }
  return null;
}
function tag(addr) {
  const m = modOf(addr);
  return m ? m.name + '+0x' + m.off.toString(16) : addr.toString();
}

send({ t: 'hello', pid: Process.id, arch: Process.arch, ptrSize: Process.pointerSize,
       main: { name: main.name, base: main.base.toString(), size: main.size } });

// ---------- locate syscall stubs inside velext -----------------------------
const stubs = [];
{
  const ranges = Process.enumerateRanges('r-x').filter(r =>
      r.base.compare(main.base) >= 0 &&
      r.base.compare(main.base.add(main.size)) < 0);
  for (const r of ranges) {
    let p = r.base;
    const end = r.base.add(r.size);
    while (p.compare(end) < 0) {
      const left = end.sub(p).toInt32();
      const chunk = Math.min(left, 0x200000);
      let hits = [];
      try { hits = Memory.scanSync(p, chunk, '0f 05'); } catch (e) { break; }
      if (!hits.length) break;
      for (const h of hits) stubs.push(h.address);
      p = hits[hits.length - 1].address.add(2);
    }
  }
}
send({ t: 'stubs', count: stubs.length,
       list: stubs.slice(0, 40).map(a => tag(a)) });

const IDS = { 0x3a: 'W', 0x3f: 'R' };  // NtWriteVirtualMemory / NtReadVirtualMemory

let events = 0, logged = 0;
const bySize = {};
const writeTargets = {};
const startMs = Date.now();
const LOG_CAP = 400;

function onSyscall(kind, src) {
  return function () {
    const id = this.context.rax.toInt32() & 0xffff;
    const nm = IDS[id];
    if (!nm) return;
    const handle = this.context.r10;
    const base = this.context.rdx;
    const size = this.context.r9.toUInt32();
    const buf = this.context.r8;
    // only interesting sizes: 36 (camera CFrame rotation), 48 (CFrame read),
    // 20 (mouse record), 12 (float3), 8 (pointer)
    if (!(size === 0x24 || size === 0x30 || size === 0x14 || size === 0xc ||
          size === 0x8 || size === 0x10 || size === 0x40))
      return;
    events++;
    const key = nm + size;
    bySize[key] = (bySize[key] || 0) + 1;
    if (nm === 'W' && size === 0x24) {
      const k = base.toString();
      writeTargets[k] = (writeTargets[k] || 0) + 1;
    }
    if (logged >= LOG_CAP) return;
    logged++;
    const rec = {
      t: 'ev', ms: Date.now() - startMs, kind: nm, src, id: '0x' + id.toString(16),
      handle: handle.toString(), base: base.toString(), size, sizeHex: '0x' + size.toString(16),
      buf: buf.toString(),
      bt: (function () {
        try {
          return Thread.backtrace(this.context, Backtracer.ACCURATE)
              .slice(0, 8).map(a => tag(a));
        } catch (e) { return ['bt-err:' + e]; }
      }).call(this),
    };
    try {
      const bytes = Memory.readByteArray(buf, Math.min(size, 0x30));
      rec.hex = Array.from(new Uint8Array(bytes))
          .map(b => ('0' + b.toString(16)).slice(-2)).join('');
    } catch (e) { rec.hex = 'read-fail'; }
    send(rec);
  };
}

let hooked = 0;
const attachErrs = [];
for (const s of stubs) {
  try { Interceptor.attach(s, { onEnter: onSyscall('syscall', tag(s)) }); hooked++; }
  catch (e) { attachErrs.push(tag(s) + ': ' + e); }
}
// Fallback: ntdll exports (in case velext calls them in some paths).
try {
  const nt = Process.getModuleByName('ntdll.dll');
  for (const n of ['NtWriteVirtualMemory', 'NtReadVirtualMemory']) {
    const p = nt.findExportByName(n);
    if (p) {
      Interceptor.attach(p, { onEnter: function () {
        // win32 args: rcx=handle rdx=base r8=buf r9=size
        const id = n.startsWith('NtWrite') ? 'W' : 'R';
        const size = this.context.r9.toUInt32();
        if (size !== 0x24 && size !== 0x30) return;
        events++;
        bySize['nt!' + id + size] = (bySize['nt!' + id + size] || 0) + 1;
        if (logged >= LOG_CAP) return;
        logged++;
        send({ t: 'ev', ms: Date.now() - startMs, kind: id, src: 'ntdll!' + n,
               base: this.context.rdx.toString(), size,
               bt: (() => { try { return Thread.backtrace(this.context, Backtracer.ACCURATE)
                     .slice(0, 8).map(a => tag(a)); } catch (e) { return ['bt-err']; } })() });
      } });
    }
  }
} catch (e) { }

setInterval(() => {
  send({ t: 'tick', sec: ((Date.now() - startMs) / 1000).toFixed(1),
         events, logged, bySize, writeTargets });
}, 1000);

send({ t: 'ready', hooked, attachErrs, bySize });
