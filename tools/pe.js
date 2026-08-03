// PE 解析 + 地址锚定工具,用于在被私服改动过的 Wow.exe 里定位 Lua 函数
const fs = require('fs');

function parsePE(buf) {
  const e_lfanew = buf.readUInt32LE(0x3C);
  if (buf.readUInt32LE(e_lfanew) !== 0x00004550) throw new Error('not PE');
  const fh = e_lfanew + 4;
  const numSections = buf.readUInt16LE(fh + 2);
  const timeStamp = buf.readUInt32LE(fh + 4);
  const sizeOptHdr = buf.readUInt16LE(fh + 16);
  const oh = fh + 20;
  const magic = buf.readUInt16LE(oh);
  const imageBase = buf.readUInt32LE(oh + 28);
  const entry = buf.readUInt32LE(oh + 16);
  const secTab = oh + sizeOptHdr;
  const sections = [];
  for (let i = 0; i < numSections; i++) {
    const p = secTab + i * 40;
    sections.push({
      name: buf.toString('latin1', p, p + 8).replace(/\0+$/, ''),
      vsize: buf.readUInt32LE(p + 8),
      va: buf.readUInt32LE(p + 12),
      rawSize: buf.readUInt32LE(p + 16),
      rawPtr: buf.readUInt32LE(p + 20),
      chars: buf.readUInt32LE(p + 36),
    });
  }
  return { imageBase, entry, magic, timeStamp, sections };
}

// RVA -> 文件偏移
function rva2off(pe, rva) {
  for (const s of pe.sections) {
    if (rva >= s.va && rva < s.va + Math.max(s.vsize, s.rawSize)) {
      const d = rva - s.va;
      if (d >= s.rawSize) return null; // 落在未初始化区
      return s.rawPtr + d;
    }
  }
  return null;
}
function va2off(pe, va) { return rva2off(pe, va - pe.imageBase); }
function off2va(pe, off) {
  for (const s of pe.sections) {
    if (off >= s.rawPtr && off < s.rawPtr + s.rawSize) {
      return pe.imageBase + s.va + (off - s.rawPtr);
    }
  }
  return null;
}

module.exports = { parsePE, rva2off, va2off, off2va };

if (require.main === module) {
  const buf = fs.readFileSync(process.argv[2]);
  const pe = parsePE(buf);
  console.log('文件大小   :', buf.length);
  console.log('ImageBase  : 0x' + pe.imageBase.toString(16).toUpperCase());
  console.log('EntryPoint : 0x' + (pe.imageBase + pe.entry).toString(16).toUpperCase());
  console.log('Magic      : 0x' + pe.magic.toString(16), pe.magic === 0x10b ? '(PE32)' : '(PE32+)');
  console.log('链接时间戳 :', new Date(pe.timeStamp * 1000).toISOString(), '(0x' + pe.timeStamp.toString(16) + ')');
  console.log('\n节表:');
  console.log('  名称       VA(虚拟地址)   VSize      RawPtr     RawSize    属性');
  for (const s of pe.sections) {
    const flags = [];
    if (s.chars & 0x20000000) flags.push('X');
    if (s.chars & 0x40000000) flags.push('R');
    if (s.chars & 0x80000000) flags.push('W');
    if (s.chars & 0x00000020) flags.push('CODE');
    if (s.chars & 0x00000040) flags.push('DATA');
    console.log(
      '  ' + s.name.padEnd(10),
      ('0x' + (pe.imageBase + s.va).toString(16).toUpperCase()).padEnd(14),
      ('0x' + s.vsize.toString(16)).padEnd(10),
      ('0x' + s.rawPtr.toString(16)).padEnd(10),
      ('0x' + s.rawSize.toString(16)).padEnd(10),
      flags.join('|')
    );
  }
}
