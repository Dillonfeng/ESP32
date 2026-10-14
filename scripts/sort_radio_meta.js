// sort_radio_meta.js  --  Option A: bake the 海燕 app's pinyin order into radio_meta.bin
//
// The ESP32 currently shows each region in the raw channels.json order (no sort).
// The 海燕 app sorts every region with electronStationSort (ICU localeCompare zh-CN,
// with per-province cityOrder). This script re-sorts the DEPLOYED radio_meta.bin in
// place so 全部 + every region view match the app byte-for-byte.
//
// WHY Node.js (not Python): the app's sort is String.localeCompare('zh-CN') = ICU
// Unicode Collation. Only V8's localeCompare reproduces it exactly (verified: the
// 32 中央 stations sort to the same order as both app screenshots).
//
// WHAT IT CHANGES:
//   * allSeq (the "全部" sequence, also the source for every regionCat) is re-sorted:
//       - channels only (non-user sids) -- exactly like the app (user lives in 个人)
//       - grouped by region, regions ordered by ELECTRON_PROVINCE_ORDER
//       - within a region: electronStationSort (cityOrder then localeCompare zh-CN)
//   * header allCount updated to the new length (1652).
//   * fav / hist / extra sections are copied BYTE-FOR-BYTE (个人/收藏/历史 untouched).
//
// SAFETY:
//   * The source multiset is the non-user sids already present in the deployed allSeq
//     (so no station is added/dropped vs. what 全部 currently shows, minus user).
//   * A timestamped .bak of the original radio_meta.bin is written before overwriting.
//
// Run:  node scripts/sort_radio_meta.js
// Verify: node scripts/verify_radio_meta.js

'use strict';
const fs = require('fs');
const path = require('path');

const BASE = path.join(__dirname, '..');
const META = path.join(BASE, 'data', 'radio_meta.bin');
const BINS = path.join(BASE, 'data', 'stations.bin');

// ===================== 100% verbatim from 海燕 mobile/www/app.js =====================
const ELECTRON_PROVINCE_ORDER = [
  '全国', '中央', '电视伴音', '国际',
  '北京', '上海', '天津', '重庆',
  '香港', '澳门', '台湾',
  '河北', '山西', '辽宁', '吉林', '黑龙江',
  '江苏', '浙江', '安徽', '福建', '江西', '山东',
  '河南', '湖北', '湖南', '广东', '广西', '海南',
  '四川', '贵州', '云南', '西藏',
  '陕西', '甘肃', '青海', '宁夏', '新疆',
  '内蒙古', '海外', '个人', '其它'
];
const ELECTRON_CITY_ORDER = {
  '北京': ['北京'],
  '上海': ['上海'],
  '天津': ['天津'],
  '重庆': ['重庆', '渝中', '江北', '南岸', '北碚', '万盛', '双桥', '渝北', '巴南'],
  '河北': ['石家庄', '唐山', '秦皇岛', '邯郸', '邢台', '保定', '张家口', '承德', '沧州', '廊坊', '衡水'],
  '山西': ['太原', '大同', '阳泉', '长治', '晋城', '朔州', '晋中', '运城', '忻州', '临汾', '吕梁'],
  '辽宁': ['沈阳', '大连', '鞍山', '抚顺', '本溪', '丹东', '锦州', '营口', '阜新', '辽阳', '盘锦', '铁岭', '朝阳', '葫芦岛'],
  '吉林': ['长春', '吉林', '四平', '辽源', '通化', '白山', '松原', '白城', '延边'],
  '黑龙江': ['哈尔滨', '齐齐哈尔', '鸡西', '鹤岗', '双鸭山', '大庆', '伊春', '佳木斯', '七台河', '牡丹江', '黑河', '绥化', '大兴安岭'],
  '江苏': ['南京', '无锡', '徐州', '常州', '苏州', '南通', '连云港', '淮安', '盐城', '扬州', '镇江', '泰州', '宿迁'],
  '浙江': ['杭州', '宁波', '温州', '嘉兴', '湖州', '绍兴', '金华', '衢州', '舟山', '台州', '丽水'],
  '安徽': ['合肥', '芜湖', '蚌埠', '淮南', '马鞍山', '淮北', '铜陵', '安庆', '黄山', '滁州', '阜阳', '宿州', '巢湖', '六安', '亳州', '池州', '宣城'],
  '福建': ['福州', '厦门', '莆田', '三明', '泉州', '漳州', '南平', '龙岩', '宁德'],
  '江西': ['南昌', '景德镇', '萍乡', '九江', '新余', '鹰潭', '赣州', '吉安', '宜春', '抚州', '上饶'],
  '山东': ['济南', '青岛', '淄博', '枣庄', '东营', '烟台', '潍坊', '济宁', '泰安', '威海', '日照', '莱芜', '临沂', '德州', '聊城', '滨州', '菏泽'],
  '河南': ['郑州', '开封', '洛阳', '平顶山', '安阳', '鹤壁', '新乡', '焦作', '濮阳', '许昌', '漯河', '三门峡', '南阳', '商丘', '信阳', '周口', '驻马店'],
  '湖北': ['武汉', '黄石', '十堰', '宜昌', '襄樊', '鄂州', '荆门', '孝感', '荆州', '黄冈', '咸宁', '随州', '恩施'],
  '湖南': ['长沙', '株洲', '湘潭', '衡阳', '邵阳', '岳阳', '常德', '张家界', '益阳', '郴州', '永州', '怀化', '娄底', '湘西'],
  '广东': ['广州', '韶关', '深圳', '珠海', '汕头', '佛山', '江门', '湛江', '茂名', '肇庆', '惠州', '梅州', '汕尾', '河源', '阳江', '清远', '东莞', '中山', '潮州', '揭阳', '云浮'],
  '广西': ['南宁', '柳州', '桂林', '梧州', '北海', '防城港', '钦州', '贵港', '玉林', '百色', '贺州', '河池', '来宾', '崇左'],
  '海南': ['海口', '三亚'],
  '四川': ['成都', '自贡', '攀枝花', '泸州', '德阳', '绵阳', '广元', '遂宁', '内江', '乐山', '南充', '眉山', '宜宾', '广安', '达州', '雅安', '巴中', '资阳', '阿坝', '甘孜', '凉山'],
  '贵州': ['贵阳', '六盘水', '遵义', '安顺', '铜仁', '毕节', '黔西南', '黔东南', '黔南'],
  '云南': ['昆明', '曲靖', '玉溪', '保山', '昭通', '丽江', '普洱', '临沧', '楚雄', '红河', '文山', '西双版纳', '大理', '德宏', '怒江', '迪庆'],
  '西藏': ['拉萨', '昌都', '山南', '日喀则', '那曲', '阿里', '林芝'],
  '陕西': ['西安', '铜川', '宝鸡', '咸阳', '渭南', '延安', '汉中', '榆林', '安康', '商洛'],
  '甘肃': ['兰州', '嘉峪关', '金昌', '白银', '天水', '武威', '张掖', '平凉', '酒泉', '庆阳', '定西', '陇南', '临夏', '甘南'],
  '青海': ['西宁', '海东', '海北', '黄南', '海南', '果洛', '玉树', '海西'],
  '宁夏': ['银川', '石嘴山', '吴忠', '固原', '中卫'],
  '新疆': ['乌鲁木齐', '克拉玛依', '吐鲁番', '哈密', '昌吉', '博尔塔拉', '巴音郭楞', '阿克苏', '克孜勒苏', '喀什', '和田', '伊犁', '塔城', '阿勒泰'],
  '内蒙古': ['呼和浩特', '包头', '乌海', '赤峰', '通辽', '鄂尔多斯', '呼伦贝尔', '巴彦淖尔', '乌兰察布', '兴安', '锡林郭勒', '阿拉善'],
  '香港': ['香港'],
  '澳门': ['澳门'],
  '台湾': ['台北', '高雄', '台中', '台南', '新竹', '嘉义'],
  '全国': [],
  '中央': [],
  '国际': [],
  '电视伴音': [],
  '海外': []
};
function electronStationSort(regionName, stationsArr) {
  const stationsCopy = stationsArr.slice();
  stationsCopy.sort(function (a, b) {
    var nameA = a.name, nameB = b.name;
    if (!ELECTRON_CITY_ORDER[regionName]) {
      return nameA.localeCompare(nameB, 'zh-CN');
    }
    var cities = ELECTRON_CITY_ORDER[regionName];
    var idxA = -1, idxB = -1;
    for (var i = 0; i < cities.length; i++) { if (nameA.indexOf(cities[i]) !== -1) { idxA = i; break; } }
    for (var i = 0; i < cities.length; i++) { if (nameB.indexOf(cities[i]) !== -1) { idxB = i; break; } }
    if (idxA !== idxB) return idxA - idxB;
    return nameA.localeCompare(nameB, 'zh-CN');
  });
  return stationsCopy;
}
// V161: garbled region name -> 其它 (verbatim from app.js)
function isGarbledRegion(name) {
  if (!name) return false;
  const s = String(name);
  if (s.indexOf('\uFFFD') >= 0) return true;
  let hasText = false;
  for (const ch of s) {
    const cp = ch.codePointAt(0);
    if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x30 && cp <= 0x39) || (cp >= 0x41 && cp <= 0x5A) ||
        (cp >= 0x61 && cp <= 0x7A)) { hasText = true; break; }
  }
  return !hasText;
}

// ============================ binary helpers ============================
function u16(buf, o) { return buf[o] | (buf[o + 1] << 8); }
function u32(buf, o) { return buf[o] | (buf[o + 1] << 8) | (buf[o + 2] << 16) | (buf[o + 3] << 24); }

// ---------- parse stations.bin -> bin sid (0..binTotal-1) : (name, region) ----------
function parseStationsBin() {
  const b = fs.readFileSync(BINS);
  const magic = b.slice(0, 8).toString('utf8');
  if (magic !== 'ESP32RDB') throw new Error('bad stations.bin magic: ' + magic);
  const numRegions = u16(b, 9);
  const regionIdxOffset = u32(b, 15);
  const regionNames = [], regionFirst = [], regionCount = [];
  for (let i = 0; i < numRegions; i++) {
    const off = regionIdxOffset + i * 28;
    regionNames.push(b.slice(off, off + 16).toString('utf8').replace(/\0+$/, ''));
    regionFirst.push(u32(b, off + 16));
    regionCount.push(u16(b, off + 20));
  }
  const sid = { name: {}, region: {} };
  let s = 0;
  for (let i = 0; i < numRegions; i++) {
    let pos = regionFirst[i]; const cnt = regionCount[i];
    for (let k = 0; k < cnt; k++) {
      const nl = b[pos]; pos += 1;
      sid.name[s] = b.slice(pos, pos + nl).toString('utf8'); pos += nl;
      const ul = u16(b, pos); pos += 2; pos += ul;        // url
      const ridx = b[pos]; pos += 1;
      const cl = b[pos]; pos += 1; pos += cl;            // cat
      const fl = b[pos]; pos += 1; pos += fl;            // freq
      pos += 3;                                          // rgb
      sid.region[s] = regionNames[ridx];
      s++;
    }
  }
  return { total: s, name: sid.name, region: sid.region };
}

// ---------- parse radio_meta.bin ----------
function parseRadioMeta() {
  const m = fs.readFileSync(META);
  const magic = m.slice(0, 8).toString('utf8');
  if (magic !== 'ESP32RDT') throw new Error('bad radio_meta.bin magic: ' + magic);
  const header = {
    version: m[8],
    binTotal: u16(m, 9), extraCount: u16(m, 11), userCount: u16(m, 13),
    favCount: u16(m, 15), histCount: u16(m, 17), allCount: u16(m, 19),
    reserved: m[21]
  };
  let off = 22;
  const allSeq = [];
  for (let i = 0; i < header.allCount; i++) { allSeq.push(u16(m, off)); off += 2; }
  const favOff = off;
  off += header.favCount * 2;
  const histOff = off;
  off += header.histCount * 2;
  const extraOff = off;
  const extra = { name: {}, region: {}, isUser: {} };
  let p = extraOff;
  for (let i = 0; i < header.extraCount; i++) {
    const nl = m[p]; p += 1;
    extra.name[header.binTotal + i] = m.slice(p, p + nl).toString('utf8'); p += nl;
    const ul = u16(m, p); p += 2; p += ul;               // url
    const rl = m[p]; p += 1;
    extra.region[header.binTotal + i] = m.slice(p, p + rl).toString('utf8'); p += rl;
    const cl = m[p]; p += 1; p += cl;                    // cat
    const fl = m[p]; p += 1; p += fl;                    // freq
    p += 3;                                              // rgb
    extra.isUser[header.binTotal + i] = m[p] !== 0; p += 1;
  }
  return { header, allSeq, favOff, histOff, extraOff, extra, raw: m };
}
// ============================ main ============================
function main() {
  const sb = parseStationsBin();
  const rm = parseRadioMeta();
  const H = rm.header;

  if (sb.total !== H.binTotal) throw new Error('stations.bin total ' + sb.total + ' != meta binTotal ' + H.binTotal);

  const nameOf = s => s < H.binTotal ? sb.name[s] : rm.extra.name[s];
  const regionOfRaw = s => s < H.binTotal ? sb.region[s] : rm.extra.region[s];
  const isUser = s => s < H.binTotal ? false : rm.extra.isUser[s];
  const regionOf = s => { const r = regionOfRaw(s); return isGarbledRegion(r) ? '其它' : r; };

  // source multiset = non-user sids present in the current allSeq (preserves dups)
  const source = rm.allSeq.filter(s => !isUser(s));

  const groups = {};
  for (const s of source) {
    const r = regionOf(s);
    if (!groups[r]) groups[r] = [];
    groups[r].push({ sid: s, name: nameOf(s) });
  }
  for (const r of Object.keys(groups)) groups[r] = electronStationSort(r, groups[r]);
  const regionNames = Object.keys(groups).sort(function (a, b) {
    const ia = ELECTRON_PROVINCE_ORDER.indexOf(a);
    const ib = ELECTRON_PROVINCE_ORDER.indexOf(b);
    if (ia !== ib) return ia - ib;
    return a.localeCompare(b, 'zh-CN');
  });
  const newAllSeq = [];
  for (const r of regionNames) for (const it of groups[r]) newAllSeq.push(it.sid);

  // sanity: newAllSeq must be a permutation of source
  const a = source.slice().sort((x, y) => x - y);
  const b2 = newAllSeq.slice().sort((x, y) => x - y);
  if (a.length !== b2.length || a.some((v, i) => v !== b2[i])) {
    throw new Error('newAllSeq is not a permutation of source!');
  }

  const m = rm.raw;
  const newHdr = Buffer.alloc(22);
  m.copy(newHdr, 0, 0, 22);
  newHdr[19] = newAllSeq.length & 0xff; newHdr[20] = (newAllSeq.length >> 8) & 0xff;
  const seqBytes = Buffer.alloc(newAllSeq.length * 2);
  newAllSeq.forEach((s, i) => seqBytes.writeUInt16LE(s, i * 2));
  const favBytes = m.slice(rm.favOff, rm.favOff + H.favCount * 2);
  const histBytes = m.slice(rm.histOff, rm.histOff + H.histCount * 2);
  const extraBytes = m.slice(rm.extraOff);
  const out = Buffer.concat([newHdr, seqBytes, favBytes, histBytes, extraBytes]);

  const ts = new Date().toISOString().replace(/[:.]/g, '-');
  const bak = META + '.bak-' + ts;
  fs.copyFileSync(META, bak);
  fs.writeFileSync(META, out);

  console.log('WROTE ' + META + ' (' + out.length + ' bytes), backup -> ' + path.basename(bak));
  console.log('  allCount: ' + H.allCount + ' -> ' + newAllSeq.length + '   (channels only, user now 个人-only, matching app)');
  console.log('  region blocks: ' + regionNames.map(r => r + '=' + groups[r].length).join('  '));

  const zy = newAllSeq.filter(s => regionOf(s) === '中央').map(s => nameOf(s));
  console.log('\n=== 中央 after sort (' + zy.length + ') ===');
  zy.forEach((n, i) => console.log(String(i + 1).padStart(2), n));
}

main();

