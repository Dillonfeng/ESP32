// verify_radio_meta.js -- re-parse the (new) radio_meta.bin + backup and assert correctness.
'use strict';
const fs = require('fs');
const path = require('path');
const BASE = path.join(__dirname, '..');
const META = path.join(BASE, 'data', 'radio_meta.bin');

function u16(b, o) { return b[o] | (b[o + 1] << 8); }
function u32(b, o) { return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24); }

function parse(b) {
  const H = { magic: b.slice(0, 8).toString('utf8'), version: b[8], binTotal: u16(b, 9),
    extraCount: u16(b, 11), userCount: u16(b, 13), favCount: u16(b, 15),
    histCount: u16(b, 17), allCount: u16(b, 19), reserved: b[21] };
  let off = 22;
  const allSeq = [];
  for (let i = 0; i < H.allCount; i++) { allSeq.push(u16(b, off)); off += 2; }
  const fav = b.slice(off); off += H.favCount * 2;
  const hist = b.slice(off); off += H.histCount * 2;
  const extra = b.slice(off);
  return { H, allSeq, fav, hist, extra };
}

// stations.bin name/region per bin sid
function stationInfo() {
  const sb = fs.readFileSync(path.join(BASE, 'data', 'stations.bin'));
  const nr = u16(sb, 9), ro = u32(sb, 15);
  const rn = [], rf = [], rc = [];
  for (let i = 0; i < nr; i++) { const o = ro + i * 28; rn.push(sb.slice(o, o + 16).toString('utf8').replace(/\0+$/, '')); rf.push(u32(sb, o + 16)); rc.push(u16(sb, o + 20)); }
  const name = {}, region = {};
  let s = 0;
  for (let i = 0; i < nr; i++) { let pos = rf[i]; const cnt = rc[i];
    for (let k = 0; k < cnt; k++) { const nl = sb[pos]; pos += 1; name[s] = sb.slice(pos, pos + nl).toString('utf8'); pos += nl;
      const ul = u16(sb, pos); pos += 2 + ul; const ridx = sb[pos]; pos += 1;
      const cl = sb[pos]; pos += 1 + cl; const fl = sb[pos]; pos += 1 + fl; pos += 3; region[s] = rn[ridx]; s++; } }
  return { total: s, name, region };
}

const st = stationInfo();
const cur = parse(fs.readFileSync(META));
// find the newest backup
const baks = fs.readdirSync(path.join(BASE, 'data')).filter(x => x.startsWith('radio_meta.bin.bak-')).sort();
const old = parse(fs.readFileSync(path.join(BASE, 'data', baks[baks.length - 1])));

let ok = true;
const fail = m => { ok = false; console.log('  FAIL: ' + m); };
const pass = m => console.log('  OK:   ' + m);

console.log('=== header (new) ===');
console.log('  ' + JSON.stringify(cur.H));
console.log('=== header (backup/old) ===');
console.log('  ' + JSON.stringify(old.H));

if (cur.H.magic !== 'ESP32RDT') fail('bad magic'); else pass('magic ESP32RDT');
if (cur.H.version !== 1) fail('version != 1'); else pass('version == 1');
if (cur.H.binTotal !== st.total) fail('binTotal != stations.bin total'); else pass('binTotal == stations.bin total (' + st.total + ')');
if (cur.H.allCount !== cur.allSeq.length) fail('allCount != allSeq.length'); else pass('allCount == allSeq.length (' + cur.allCount + ')');

console.log('\n=== byte-for-byte: fav / hist / extra vs backup ===');
if (Buffer.compare(cur.fav, old.fav) === 0) pass('fav section identical (' + old.fav.length + 'B)'); else fail('fav differs');
if (Buffer.compare(cur.hist, old.hist) === 0) pass('hist section identical (' + old.hist.length + 'B)'); else fail('hist differs');
if (Buffer.compare(cur.extra, old.extra) === 0) pass('extra section identical (' + old.extra.length + 'B)'); else fail('extra differs');

console.log('\n=== allSeq content vs backup (must be a subset: no user sids added, only re-order + drop user) ===');
const oldSet = new Set(old.allSeq);
const newSet = new Set(cur.allSeq);
const added = [...newSet].filter(s => !oldSet.has(s));
const removed = [...oldSet].filter(s => !newSet.has(s));
if (added.length === 0) pass('no sids added'); else fail('added sids: ' + added.join(','));
// removed should be exactly the user sids (bin sids must all be preserved)
const removedBin = removed.filter(s => s < cur.H.binTotal);
if (removedBin.length === 0) pass('all ' + cur.H.binTotal + ' bin (channel) sids preserved'); else fail('bin sids dropped: ' + removedBin.join(','));
const removedExtra = removed.filter(s => s >= cur.H.binTotal);
if (removedExtra.length > 0) console.log('  (note: user/extra sids removed from 全部 = ' + removedExtra.length + ' -- expected, they move to 个人)');

console.log('\n=== region block counts (new allSeq) ===');
const cnt = {};
for (const s of cur.allSeq) { const r = st.region[s]; cnt[r] = (cnt[r] || 0) + 1; }
let sum = 0; for (const r of Object.keys(cnt)) { sum += cnt[r]; console.log('  ' + r + '=' + cnt[r]); }
if (sum === cur.allCount) pass('region counts sum to allCount (' + sum + ')'); else fail('region sum ' + sum + ' != allCount');

console.log('\n=== 中央 ordered names (new) ===');
const zy = cur.allSeq.filter(s => st.region[s] === '中央').map(s => st.name[s]);
zy.forEach((n, i) => console.log('  ' + String(i + 1).padStart(2) + ' ' + n));
if (zy.length === 32) pass('中央 count == 32'); else fail('中央 count ' + zy.length + ' != 32');

// expected 中央 order (from the verified app screenshots / _zy_sort_test)
const expected = ['故城经典音乐FM90.5','环球资讯','经典音乐广播','经济之声','南海之声','轻松调频','台海之声','音乐之声','阅读之声','中央文艺之声','中央中国之声','CGTN - English','CGTN Arabic','CGTN Documentary','CGTN Radio','CGTN Russian','CNR-1 中国之声','CNR-1 中国之声','CNR-1 中国之声','CNR-1 中国之声','CNR-1 中国之声','CNR-10 老年之声','CNR-11','CNR-13','CNR-15 中国交通广播','CNR-17','CNR-2 经济之声','CNR-2 经济之声','CNR-3 音乐之声','CNR-4 经典音乐广播','CNR-8 民族之声','CNR-9 文艺之声'];
// compare by leading chars (some names have trailing garbled bytes); compare first 6 code units
const lead = a => a.slice(0, 6);
let match = true;
for (let i = 0; i < Math.max(zy.length, expected.length); i++) {
  if (lead(zy[i] || '') !== lead(expected[i] || '')) { match = false; fail('中央 pos ' + (i + 1) + ': got "' + (zy[i]||'') + '" want "' + (expected[i]||'') + '"'); break; }
}
if (match) pass('中央 order matches app screenshots (first-6 chars)');

console.log('\n=== RESULT: ' + (ok ? 'ALL PASS' : 'HAS FAILURES') + ' ===');
process.exit(ok ? 0 : 1);
