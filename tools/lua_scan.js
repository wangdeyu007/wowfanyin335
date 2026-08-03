// 全 .text 段扫描:统计对 Lua 库区间的 call,并用「调用方清栈字节数」推断参数个数
const fs = require('fs');
const { parsePE } = require('./pe.js');

const buf = fs.readFileSync(process.argv[2]);
const pe = parsePE(buf);
const LO = parseInt(process.argv[3] || '0x84C000', 16);
const HI = parseInt(process.argv[4] || '0x852000', 16);

const t = pe.sections.find(s => s.name === '.text');
const base = pe.imageBase + t.va;

// target -> { total, cleanup: {bytes: count} }
const stats = new Map();

for (let i = 0; i < t.rawSize - 5; i++) {
  if (buf[t.rawPtr + i] !== 0xE8) continue;
  const rel = buf.readInt32LE(t.rawPtr + i + 1);
  const site = base + i;
  const target = (site + 5 + rel) >>> 0;
  if (target < LO || target >= HI) continue;

  if (!stats.has(target)) stats.set(target, { total: 0, cleanup: new Map() });
  const s = stats.get(target);
  s.total++;

  // look at bytes right after the call for `add esp, imm8` (83 C4 xx)
  const p = t.rawPtr + i + 5;
  let key = '?';
  if (buf[p] === 0x83 && buf[p + 1] === 0xC4) key = String(buf[p + 2]);
  else if (buf[p] === 0x81 && buf[p + 1] === 0xC4) key = String(buf.readUInt32LE(p + 2));
  else if (buf[p] === 0x59) key = '4(pop ecx)';
  else if (buf[p] === 0x5A) key = '4(pop edx)';
  s.cleanup.set(key, (s.cleanup.get(key) || 0) + 1);
}

const rows = [...stats.entries()].sort((a, b) => a[0] - b[0]);
console.log(`Lua 库区间 [0x${LO.toString(16)}, 0x${HI.toString(16)}) 内被调用的函数,共 ${rows.length} 个\n`);
console.log('地址        被调用次数  清栈字节数分布(字节数 x 次数) -> 推断参数个数');
for (const [addr, s] of rows) {
  if (s.total < 3) continue; // 过滤噪声
  const dist = [...s.cleanup.entries()].sort((a, b) => b[1] - a[1]);
  const top = dist[0][0];
  const nArgs = /^\d+$/.test(top) ? (Number(top) / 4) : '?';
  console.log(
    '0x' + addr.toString(16).toUpperCase().padEnd(9),
    String(s.total).padStart(6) + '     ',
    dist.map(([k, v]) => `${k}x${v}`).join(' ').padEnd(34),
    '-> ' + nArgs + ' 个 dword 参数'
  );
}
