// dump 指定 VA 处的函数字节,并提取 call rel32 目标
const fs = require('fs');
const { parsePE, va2off, off2va } = require('./pe.js');

const buf = fs.readFileSync(process.argv[2]);
const pe = parsePE(buf);
const startVA = parseInt(process.argv[3], 16);
const len = parseInt(process.argv[4] || '160', 10);

const textSec = pe.sections.find(s => s.name === '.text');
const textLo = pe.imageBase + textSec.va;
const textHi = textLo + textSec.vsize;

const off = va2off(pe, startVA);
if (off === null) { console.error('VA 不在文件内'); process.exit(1); }

const bytes = buf.slice(off, off + len);

// 十六进制 dump,带 VA
console.log(`--- 字节 dump @ VA 0x${startVA.toString(16).toUpperCase()} (文件偏移 0x${off.toString(16)}) ---`);
for (let i = 0; i < bytes.length; i += 16) {
  const chunk = bytes.slice(i, i + 16);
  const hex = [...chunk].map(b => b.toString(16).padStart(2, '0')).join(' ');
  console.log('0x' + (startVA + i).toString(16).toUpperCase() + '  ' + hex);
}

// 提取 call rel32 (E8) 与 jmp rel32 (E9)
console.log('\n--- call/jmp rel32 目标 ---');
for (let i = 0; i < bytes.length - 4; i++) {
  const op = bytes[i];
  if (op !== 0xE8 && op !== 0xE9) continue;
  const rel = bytes.readInt32LE(i + 1);
  const target = (startVA + i + 5 + rel) >>> 0;
  if (target >= textLo && target < textHi) {
    console.log(
      `  @0x${(startVA + i).toString(16).toUpperCase()}  ` +
      `${op === 0xE8 ? 'call' : 'jmp '} 0x${target.toString(16).toUpperCase()}`
    );
  }
}

// 找 ret(C3) 位置,粗略判断函数结尾
console.log('\n--- ret (C3) 出现位置 ---');
for (let i = 0; i < bytes.length; i++) {
  if (bytes[i] === 0xC3) {
    const next = bytes[i + 1];
    console.log(`  @0x${(startVA + i).toString(16).toUpperCase()}` +
      (next === 0xCC ? '  (后跟 int3 填充 -> 很可能是函数结尾)' : ''));
  }
}
