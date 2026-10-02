# -*- coding: utf-8 -*-
import os
P = r'd:\ESP32-S3\src\main.cpp'
S = r'd:\ESP32-S3\scripts'
src = open(P, 'rb').read().decode('utf-8').replace('\r\n', '\n')   # normalize to LF for matching
orig_len = len(src)
p1 = open(os.path.join(S, '_radio_part1.cpp'), 'r', encoding='utf-8').read().replace('\r\n', '\n')
p2 = open(os.path.join(S, '_radio_part2.cpp'), 'r', encoding='utf-8').read().replace('\r\n', '\n')
p3 = open(os.path.join(S, '_radio_part3.cpp'), 'r', encoding='utf-8').read().replace('\r\n', '\n')
def ok(t): print('OK  ' + t)
def repl_once(s, old, new, tag):
    assert s.count(old) == 1, tag + ' matched %d (need 1)' % s.count(old)
    ok(tag); return s.replace(old, new, 1)
def func_end(s, i):
    j = s.find('{', i); depth = 0; k = j
    while k < len(s):
        if s[k] == '{': depth += 1
        elif s[k] == '}':
            depth -= 1
            if depth == 0: break
        k += 1
    return k + 1
def remove_span(s, a_sig, b_sig, tag):
    i = s.find(a_sig); assert i != -1, tag + ': a not found'
    j = s.find(b_sig, i); assert j != -1, tag + ': b not found'
    e = func_end(s, j)
    ok(tag + ' (remove %d bytes)' % (e - i)); return s[:i] + s[e:]

# 1) geometry + globals
i = src.find('#define RADIO_SIDEBAR_W    70'); j = src.find('int listScrollTop = 0;')
assert i != -1 and j != -1
ok('globals (remove %d bytes)' % (j - i))
src = src[:i] + p1 + src[j + len('int listScrollTop = 0;'):]

# 2) old data funcs (loadRadioDB+loadRegionStations+readStationURL) -> p2
i = src.find('bool loadRadioDB() {')
e = func_end(src, src.find('String readStationURL(uint32_t binOffset) {'))
ok('old data funcs (remove %d bytes)' % (e - i))
src = src[:i] + p2 + src[e:]

# 3) remove old playStationAt(int listIdx) + old stopRadio()
i = src.find('void playStationAt(int listIdx) {')
e_play = func_end(src, i)
i_stop = src.find('void stopRadio() {', e_play)
e_stop = func_end(src, i_stop)
ok('old playStationAt+stopRadio (remove %d bytes)' % (e_stop - i))
src = src[:i] + src[e_stop:]

# 4) remove old favorites funcs (loadFavorites..toggleFavorite)
i = src.find('void loadFavorites() {')
e = func_end(src, src.find('void toggleFavorite(uint32_t binOffset) {'))
ok('old favorites funcs (remove %d bytes)' % (e - i))
src = src[:i] + src[e:]

# 5) old drawing funcs (drawRadioHeader..drawStationFullscreen) -> p3
i = src.find('void drawRadioHeader() {')
e = func_end(src, src.find('void drawStationFullscreen() {'))
ok('old drawing funcs (remove %d bytes)' % (e - i))
src = src[:i] + p3 + src[e:]

# 6) forward declarations
FWD_OLD = """bool loadRadioDB();
void loadRegionStations(int regionIdx);
void playStationAt(int listIdx);
void stopRadio();
String readStationURL(uint32_t binOffset);
void drawRadioRegionSidebar();
void drawRadioList();
void drawRadioHeader();
void drawRadioNowPlaying();
void drawStationRow(int rowY, StationView& st, bool playing);
void drawRadioScrollbar();
void saveFavorites();
bool isFavorited(uint32_t binOffset);
void toggleFavorite(uint32_t binOffset);
void loadFavorites();
void drawRadioPlayBar();"""
FWD_NEW = """bool loadRadioDB();
bool loadRadioMeta();
void playStationAt(int sid);
void stopRadio();
String readURLForSid(int sid);
void drawRadioRegionSidebar();
void drawRadioList();
void drawRadioHeader();
void drawStationRow(int rowY, RadioStation* st, bool playing, bool favorited);
void drawStationIcon(int cx, int cy, RadioStation* st, int size);
void drawRadioScrollbar();
void drawRadioPlayBar();"""
src = repl_once(src, FWD_OLD, FWD_NEW, 'fwd')

# 7) setup: load meta + drop loadFavorites
i = src.find('  Serial.println("Loading radio DB...");')
j = src.find('  loadFavorites();')
assert i != -1 and j != -1
ok('setup (remove %d bytes)' % (j + 16 - i))
newsetup = ('  Serial.println("Loading radio DB...");\n'
            '  bool dbOk = loadRadioDB();\n'
            '  bool metaOk = loadRadioMeta();\n'
            '  if (dbOk && metaOk) tft.printf("%d cat / %d stn\\n", radioCatCount, radioStnTotal);\n'
            '  else tft.println("DB: MISSING");\n'
            '  Serial.printf("DB cats=%d stn=%d\\n", radioCatCount, radioStnTotal);\n')
src = src[:i] + newsetup + src[j + 16:]

# 8) handleTouch scroll clamp
src = repl_once(src, 'newScroll = constrain(newScroll, 0, max(0, curRegionCount - RADIO_STATION_ROWS));',
                'newScroll = constrain(newScroll, 0, max(0, curCount() - RADIO_STATION_ROWS));', 'scrollclamp')

# 9) handleRadioTap rewrite (sidebar 4+region / playbar 3-key / scrollbar / list)
HRT = (
"void handleRadioTap(uint16_t tx, uint16_t ty) {\n"
"  // 全屏: 点任意处关闭\n"
"  if (showingFullscreen) { showingFullscreen = false; modeDrawn[MODE_RADIO] = false; return; }\n"
"  // 侧边栏 (固定 4 行 + 可滚动地区行)\n"
"  if (tx < RADIO_SIDEBAR_W) {\n"
"    int regionY = ty - RADIO_HEADER_H;\n"
"    if (regionY >= 0 && regionY < (RADIO_REGION_TOP - RADIO_HEADER_H)) {\n"
"      int c = regionY / RADIO_SPECIAL_H;\n"
"      if (c >= 0 && c < radioCatCount && c != currentCat) { currentCat = c; listScrollTop = 0; modeDrawn[MODE_RADIO] = false; }\n"
"    } else if (regionY >= (RADIO_REGION_TOP - RADIO_HEADER_H)) {\n"
"      int ry = ty - RADIO_REGION_TOP;\n"
"      int c = RADIO_SPECIAL_CNT + regionScrollTop + ry / RADIO_ROW_H_SIDE;\n"
"      if (c < radioCatCount && c != currentCat) { currentCat = c; listScrollTop = 0; modeDrawn[MODE_RADIO] = false; }\n"
"    }\n"
"    return;\n"
"  }\n"
"  // 播放栏 (三键: 上一 cx0-45 / 播放 cx0 / 下一 cx0+45, 各宽 ~44)\n"
"  if (ty >= RADIO_LIST_BOT && ty < NAV_BAR_Y) {\n"
"    int barX = RADIO_LIST_X, cx0 = barX + RADIO_LIST_W / 2;\n"
"    if (tx >= barX && tx < barX + RADIO_LIST_W) {\n"
"      int relX = tx - barX;\n"
"      int pos = -1;\n"
"      if (currentPlayingSid >= 0) for (int i = 0; i < curCount(); i++) if (curSidAt(i) == currentPlayingSid) { pos = i; break; }\n"
"      if (relX >= cx0 - 68 && relX <= cx0 - 22 && pos > 0) playStationAt(curSidAt(pos - 1));      // 上一\n"
"      else if (relX >= cx0 - 15 && relX <= cx0 + 15) { if (radioPlaying) stopRadio(); else if (pos >= 0) playStationAt(curSidAt(pos)); modeDrawn[MODE_RADIO] = false; }  // 播放/暂停\n"
"      else if (relX >= cx0 + 22 && relX <= cx0 + 68 && pos >= 0 && pos < curCount() - 1) playStationAt(curSidAt(pos + 1));  // 下一\n"
"    }\n"
"    return;\n"
"  }\n"
"  // 滚动条点击跳转\n"
"  if (ty >= RADIO_LIST_TOP && ty < RADIO_LIST_BOT && tx >= RADIO_LIST_X + RADIO_LIST_W && tx < RADIO_LIST_X + RADIO_LIST_W + RADIO_SCROLLBAR_W) {\n"
"    int count = curCount();\n"
"    int scrollRange = max(1, count - RADIO_STATION_ROWS);\n"
"    int listH = RADIO_LIST_BOT - RADIO_LIST_TOP;\n"
"    int scrollPos = (ty - RADIO_LIST_TOP) * scrollRange / listH;\n"
"    scrollPos = constrain(scrollPos, 0, max(0, count - RADIO_STATION_ROWS));\n"
"    if (scrollPos != listScrollTop) { listScrollTop = scrollPos; listScrollDirty = true; }\n"
"    return;\n"
"  }\n"
"  // 列表: 单击播放, 双击全屏\n"
"  if (ty >= RADIO_LIST_TOP && ty < RADIO_LIST_BOT && tx >= RADIO_LIST_X) {\n"
"    int listY = ty - RADIO_LIST_TOP;\n"
"    int rowIdx = listScrollTop + listY / RADIO_ROW_H;\n"
"    if (rowIdx >= 0 && rowIdx < curCount()) {\n"
"      unsigned long now = millis();\n"
"      if (now - lastTapTime < 350 && lastTapRow == rowIdx) { showingFullscreen = true; modeDrawn[MODE_RADIO] = false; }\n"
"      else { playStationAt(curSidAt(rowIdx)); listScrollDirty = true; }\n"
"      lastTapTime = now; lastTapRow = rowIdx;\n"
"    }\n"
"    return;\n"
"  }\n"
"}")
i = src.find('void handleRadioTap(uint16_t tx, uint16_t ty) {')
e = func_end(src, i)
ok('handleRadioTap (remove %d bytes)' % (e - i))
src = src[:i] + HRT + src[e:]

open(P, 'wb').write(src.replace('\n', '\r\n').encode('utf-8'))
print('WROTE main.cpp  %d -> %d bytes (diff %+d)' % (orig_len, len(src), len(src) - orig_len))
