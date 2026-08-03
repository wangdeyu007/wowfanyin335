// 扫描 Lua 代码区,找写入 tt 常量的函数(定位 lua_push* 系列)
// 特征: c7 4x 08 <imm32>  =  mov dword [reg+8], LUA_T???
const fs = require('fs');
const { parsePE } = require('./pe.js');

const buf = fs.readFileSync(process.argv[2]);
const pe = parsePE(buf);
const LO = parseInt(process.argv[3] || '0x84C000', 16);
const HI = parseInt(process.argv[4] || '0x853000', 16);
const wantTT = process.argv[5] !== undefined ? parseInt(process.argv[5], 10) : null;

const t = pe.sections.find(s => s.name === '.text');
const base = pe.imageBase + t.va;

const TT = {0:'LUA_TNIL',1:'LUA_TBOOLEAN',2:'LUA_TLIGHTUSERDATA',3:'LUA_TNUMBER',
            4:'LUA_TSTRING',5:'LUA_TTABLE',6:'LUA_TFUNCTION',7:'LUA_TUSERDATA',8:'LUA_TTHREAD'};

// 先收集函数起点:模式 55 8b ec (push ebp; mov ebp,esp),前面是 int3 填充
const starts = [];
for (let va = LO; va < HI; va++) {
  const o = t.rawPtr + (va - base);
  if (o < t.rawPtr || o + 3 > t.rawPtr + t.rawSize) continue;
  if (buf[o] === 0x55 && buf[o + 1] === 0x8b && buf[o + 2] === 0xec) {
    if (buf[o - 1] === 0xCC || buf[o - 1] === 0xC3) starts.push(va);
  }
}

function funcOf(va) {
  let best = null;
  for (const s of starts) { if (s <= va && (best === null || s > best)) best = s; }
  return best;
}

const found = [];
for (let va = LO; va < HI - 7; va++) {
  const o = t.rawPtr + (va - base);
  if (o < t.rawPtr || o + 7 > t.rawPtr + t.rawSize) continue;
  if (buf[o] !== 0xC7) continue;
  const modrm = buf[o + 1];
  // mod=01 (disp8), reg field=000 -> /0 (mov imm32)
  if ((modrm & 0xC0) !== 0x40) continue;
  if (((modrm >> 3) & 7) !== 0) continue;
  if (buf[o + 2] !== 0x08) continue; // disp8 == +8  (TValue.tt)
  const imm = buf.readUInt32LE(o + 3);
  if (imm > 8) continue;
  if (wantTT !== null && imm !== wantTT) continue;
  found.push({ va, imm, func: funcOf(va), rm: modrm & 7 });
}

const REG = ['eax','ecx','edx','ebx','esp','ebp','esi','edi'];
console.log('写入 TValue.tt 的位置(函数起点 / tt 值):\n');
const byFunc = new Map();
for (const f of found) {
  const k = f.func;
  if (!byFunc.has(k)) byFunc.set(k, []);
  byFunc.get(k).push(f);
}
for (const [fn, list] of [...byFunc.entries()].sort((a, b) => a[0] - b[0])) {
  const tts = [...new Set(list.map(x => x.imm))];
  console.log(
    '函数 0x' + (fn === null ? '????' : fn.toString(16).toUpperCase()).padEnd(9),
    ' tt = ' + tts.map(v => `${v}(${TT[v]})`).join(', ')
  );
}
