'use strict';
const fs = require('fs');
const files = [
  'D:/ESP32-S3/.pio/build/esp32-s3-devkitc-1/littlefs.bin',
  'D:/ESP32-S3/.pio/build/esp32-s3-devkitc-1/firmware.bin'
];
const out = [];
for (const f of files) {
  if (!fs.existsSync(f)) { out.push(f + ' MISSING'); continue; }
  const st = fs.statSync(f);
  out.push(f + ' size=' + st.size + ' mtime=' + st.mtime.toISOString());
}
const l = fs.readFileSync(files[0]);
const magic = Buffer.from('ESP32RDT');
let idx = 0;
const found = [];
while (true) {
  const i = l.indexOf(magic, idx);
  if (i < 0) break;
  found.push(i);
  idx = i + 1;
}
out.push('ESP32RDT occurrences in littlefs.bin: ' + found.length);
for (const o of found.slice(0, 20)) {
  const ver = l[o + 8];
  const binTotal = l.readUInt16LE(o + 9);
  const all = l.readUInt16LE(o + 19);
  const seq = [];
  for (let i = 0; i < Math.min(all, 20); i++) seq.push(l.readUInt16LE(o + 22 + i * 2));
  out.push('  at ' + o + ' ver=' + ver + ' binTotal=' + binTotal + ' all=' + all + ' first20=' + seq.join(','));
}
fs.writeFileSync('D:/ESP32-S3/_lfs_scan.txt', out.join('\n'));
console.log('done');
