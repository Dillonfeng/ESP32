// debug_allseq.js -- ASCII-only facts about the (new) radio_meta.bin
'use strict';
const fs = require('fs');
const path = require('path');
const BASE = path.join(__dirname, '..');
const META = path.join(BASE, 'data', 'radio_meta.bin');
function u16(b, o) { return b[o] | (b[o + 1] << 8); }
function u32(b, o) { return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24); }

function stn() {
  const b = fs.readFileSync(path.join(BASE, 'data', 'stations.bin'));
  const nr = u16(b, 9), ro = u32(b, 15);
  const rn = [];
  for (let i = 0; i < nr; i++) rn.push(b.slice(ro + i * 28, ro + i * 28 + 16).toString('utf8').replace(/\0+$/, ''));
  const rf = [], rc = [];
  for (let i = 0; i < nr; i++) { rf.push(u32(b, ro + i * 28 + 16)); rc.push(u16(b, ro + i * 28 + 20)); }
  const name = {}, region = {};
  let s = 0;
  for (let i = 0; i < nr; i++) { let pos = rf[i]; const cnt = rc[i];
    for (let k = 0; k < cnt; k++) { const nl = b[pos]; pos += 1; name[s] = b.slice(pos, pos + nl).toString('utf8'); pos += nl;
      const ul = u16(b, pos); pos += 2 + ul; const ridx = b[pos]; pos += 1;
      const cl = b[pos]; pos += 1 + cl; const fl = b[pos]; pos += 1 + fl; pos += 3; region[s] = rn[ridx]; s++; } }
  return { total: s, name, region, regions: rn };
}
function meta() {
  const b = fs.readFileSync(META);
  const H = { binTotal: u16(b, 9), extraCount: u16(b, 11), userCount: u16(b, 13),
    favCount: u16(b, 15), histCount: u16(b, 17), allCount: u16(b, 19) };
  let off = 22;
  const all = [];
  for (let i = 0; i < H.allCount; i++) { all.push(u16(b, off)); off += 2; }
  return { H, all };
}
const st = stn(), m = meta();
const extraSids = m.all.filter(s => s >= m.H.binTotal);
const binSids = m.all.filter(s => s < m.H.binTotal);
const zyBin = binSids.filter(s => st.region[s] === '中央');
const zyExtra = extraSids.filter(s => s >= m.H.binTotal);

const rep = {
  newAllLen: m.all.length,
  allCountHdr: m.H.allCount,
  binTotal: m.H.binTotal,
  extraCount: m.H.extraCount,
  userCount: m.H.userCount,
  numBinSidsInAll: binSids.length,
  numExtraSidsInAll: extraSids.length,
  extraSids: extraSids,
  binSidsMissingFromAll: Array.from({length: m.H.binTotal}, (_, i) => i).filter(i => !new Set(m.all).has(i)),
  zyBinCount: zyBin.length,
  zyExtraCount: zyExtra.length,
  distinctBinRegions: Object.keys(st.region).length,
};
fs.writeFileSync(path.join(BASE, '_dbg.json'), JSON.stringify(rep, null, 2));
// 中央 names in new order -> separate utf8 file
const zyNames = m.all.filter(s => (s < m.H.binTotal && st.region[s] === '中央') || s >= m.H.binTotal && st.region && st.region[s] === '中央')
  .concat([]);
// simpler: names for zyBin in allSeq order + extra 中央 in allSeq order
const zyInOrder = m.all.filter(s => (s < m.H.binTotal && st.region[s] === '中央') || (s >= m.H.binTotal));
fs.writeFileSync(path.join(BASE, '_dbg_zy.txt'),
  zyBin.map((s, i) => (i + 1) + ' BIN ' + st.name[s]).join('\n') +
  '\n--- extras in allSeq ---\n' +
  extraSids.map(s => 'EXTRA sid=' + s).join('\n'));
console.log('WROTE _dbg.json _dbg_zy.txt');
