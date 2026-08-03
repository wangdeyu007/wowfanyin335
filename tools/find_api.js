// 通过 FrameScript 注册表 {const char* name; lua_CFunction func;} 锚定 WoW 的 Lua API 地址
const fs = require('fs');
const { parsePE, va2off, off2va } = require('./pe.js');

const exePath = process.argv[2];
const names = process.argv.slice(3);
if (!names.length) { console.error('用法: node find_api.js <Wow.exe> <ApiName...>'); process.exit(1); }

const buf = fs.readFileSync(exePath);
const pe = parsePE(buf);
const textSec = pe.sections.find(s => s.name === '.text');
const textLo = pe.imageBase + textSec.va;
const textHi = textLo + textSec.vsize;

// 收集所有节的 [rawPtr, rawPtr+rawSize) 供指针扫描
function findStringVAs(name) {
  const needle = Buffer.from(name + '\0', 'latin1');
  const hits = [];
  let i = 0;
  while ((i = buf.indexOf(needle, i)) !== -1) {
    // 要求前一个字节是 0 或不可打印,避免匹配到 "XXXUnitXP" 这种子串尾部
    const prev = i > 0 ? buf[i - 1] : 0;
    if (prev === 0 || prev < 0x20) {
      const va = off2va(pe, i);
      if (va !== null) hits.push({ off: i, va });
    }
    i += 1;
  }
  return hits;
}

function findPointersTo(va) {
  const target = Buffer.alloc(4);
  target.writeUInt32LE(va >>> 0);
  const hits = [];
  let i = 0;
  while ((i = buf.indexOf(target, i)) !== -1) {
    hits.push(i);
    i += 1;
  }
  return hits;
}

for (const name of names) {
  console.log('\n===== ' + name + ' =====');
  const strs = findStringVAs(name);
  if (!strs.length) { console.log('  [!] 未找到字符串'); continue; }
  for (const s of strs) {
    console.log(`  字符串 "${name}" @ VA 0x${s.va.toString(16).toUpperCase()} (文件偏移 0x${s.off.toString(16)})`);
    const ptrs = findPointersTo(s.va);
    if (!ptrs.length) { console.log('    无指针引用'); continue; }
    for (const p of ptrs) {
      const pva = off2va(pe, p);
      // 尝试把 p 当作注册表项的 name 字段,读下一个 DWORD 作为 func
      if (p + 8 > buf.length) continue;
      const func = buf.readUInt32LE(p + 4);
      const inText = func >= textLo && func < textHi;
      const tag = inText ? '  <== 候选函数地址(在 .text 内)' : '';
      console.log(
        `    指针 @ VA 0x${(pva || 0).toString(16).toUpperCase()}` +
        `  下一DWORD = 0x${func.toString(16).toUpperCase()}${tag}`
      );
    }
  }
}
