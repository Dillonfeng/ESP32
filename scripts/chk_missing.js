// chk_missing.js -- what are the 3 missing bin sids, and were they in the backup?
'use strict';
const fs = require('fs');
const path = require('path');
const BASE = path.join(__dirname, '..');
function u16(b, o) { return b[o] | (b[o + 1] << 8); }
function u32(b, o) { return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24); }
function stn() {
  const b = fs.readFileSync(path.join(BASE, 'data', 'stations.bin'));
  const nr = u16(b, 9), ro = u32(b, 15);
  const rn = [], rf = [], rc = [];
  for (let i = 0; i < nr; i++) { rn.push(b.slice(ro + i * 28, ro + i * 28 + 16).toString('utf8').replace(/\0+$/, '')); rf.push(u32(b, ro + i * 28 + 16)); rc.push(u16(b, ro + i * 28 + 20)); }
  const name = {}, region = {};
  let s = 0;
  for (let i = 0; i < nr; i++) { let pos = rf[i]; const cnt = rc[i];
    for (let k = 0; k < cnt; k++) { const nl = b[pos]; pos += 1; name[s] = b.slice(pos, pos + nl).toString('utf8'); pos += nl;
      const ul = u16(b, pos); pos += 2 + ul; const ridx = b[pos]; pos += 1;
      const cl = b[pos]; pos += 1 + cl; const fl = b[pos]; pos += 1 + fl; pos += 3; region[s] = rn[ridx]; s++; } }
  return { total: s, name, region };
}
function allSeq(file) {
  const b = fs.readFileSync(file);
  const H = { binTotal: u16(b, 9), allCount: u16(b, 19) };
  let off = 22;
  const all = [];
  for (let i = 0; i < H.allCount; i++) { all.push(u16(b, off)); off += 2; }
  return { H, all };
}
const st = stn();
const cur = allSeq(path.join(BASE, 'data', 'radio_meta.bin'));
const baks = fs.readdirSync(path.join(BASE, 'data')).filter(x => x.startsWith('radio_meta.bin.bak-')).sort();
const bakFile = path.join(BASE, 'data', baks[baks.length - 1]);
const old = allSeq(bakFile);
const newSet = new Set(cur.all), oldSet = new Set(old.all);
const missing = [266, 281, 284];
const lines = [];
for (const s of missing) {
  lines.push('sid=' + s + '  name=' + st.name[s] + '  region=' + st.region[s]);
  lines.push('   in backup(allSeq old)? ' + oldSet.has(s) + '   |  in new(allSeq cur)? ' + newSet.has(s));
}
// also: full 中央 block in NEW allSeq order (bin + extra)
const extraNames = {};
// build extra name map from cur file extras section is complex; reuse debug: CNR-8=1652? Actually list sids>=binTotal in newAll with index
const extraInNew = cur.all.filter(s => s >= cur.H.binTotal);
lines.push('extra sids in NEW allSeq (>=binTotal): ' + JSON.stringify(extraInNew));
lines.push('their positions in NEW allSeq: ' + JSON.stringify(cur.all.map((s, i) => [s, i]).filter(([s]) => s >= cur.H.binTotal)));
// 32-block: sids whose region is 中央 (bin) OR in extraInNew, in allSeq order
const zyInNew = cur.all.filter(s => (s < cur.H.binTotal && st.region[s] === '中央') || s >= cur.H.binTotal);
lines.push('32 中央 block in NEW allSeq order (count=' + zyInNew.length + '): ' + JSON.stringify(zyInNew));
fs.writeFileSync(path.join(BASE, '_chk_missing.txt'), lines.join('\n'));
console.log('WROTE _chk_missing.txt');
