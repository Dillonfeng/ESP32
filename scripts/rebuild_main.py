# -*- coding: utf-8 -*-
cur = open(r'd:\ESP32-S3\src\main.cpp', 'r', encoding='utf-8').read()

# 切分
marker = '// ============================================================\n// handleRadioTap'
idx = cur.find(marker)
header = cur[:idx]
after = cur[idx:]

new_funcs = r'''
// ============================================================
// 触摸状态变量 (边沿检测)
// ============================================================
static bool touchWasDown = false;
static int touchStartX = 0, touchStartY = 0;
static int touchLastX = 0, touchLastY = 0;
static unsigned long touchStartTime = 0;
static bool touchDidScroll = false;
static int touchScrollStartScrollTop = 0;

// ============================================================
// initTFT / initTouch / drawNavBar
// ============================================================
void initTFT() {
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);
}

void initTouch() {
  tft.setTouch(touchCalib);
}

void drawNavBar() {
  tft.fillRect(0, NAV_BAR_Y, 480, NAV_BAR_HEIGHT, COLOR_NAV_BG);
  for (int i = 0; i < NAV_BTN_COUNT; i++) {
    int x = i * NAV_BTN_WIDTH;
    bool sel = (i == currentMode);
    if (sel) {
      tft.fillRect(x + 2, NAV_BAR_Y + 2, NAV_BTN_WIDTH - 4, NAV_BAR_HEIGHT - 4, COLOR_NAV_SEL);
    }
    uint16_t tc = sel ? TFT_YELLOW : TFT_WHITE;   // 选中=黄字, 未选=白字 (与 live preview 一致)
    int tw = font16.textWidth(navLabels[i]);
    font16.drawString(x + (NAV_BTN_WIDTH - tw) / 2, NAV_BAR_Y + 8, navLabels[i], tc, sel ? COLOR_NAV_SEL : COLOR_NAV_BG);
    if (i > 0) tft.drawFastVLine(x, NAV_BAR_Y + 2, NAV_BAR_HEIGHT - 4, 0x630C);
  }
}

// ============================================================
// handleTouch (边沿检测版)
// ============================================================
void handleTouch() {
  uint16_t tx, ty;
  int touched = tft.getTouch(&tx, &ty);
  bool isDown = (touched > 0);

  if (isDown && !touchWasDown) {
    touchStartX = touchLastX = tx;
    touchStartY = touchLastY = ty;
    touchStartTime = millis();
    touchDidScroll = false;
    touchScrollStartScrollTop = listScrollTop;
  }
  touchWasDown = isDown;

  if (!isDown && touchWasDown) {
    unsigned long dur = millis() - touchStartTime;
    int dy = touchLastY - touchStartY;
    int dx = touchLastX - touchStartX;
    touchWasDown = false;
    touchStartTime = 0;

    if (!touchDidScroll && dur < 800 && abs(dy) < 15 && abs(dx) < 15) {
      if (touchStartY >= NAV_BAR_Y) {
        int btnIdx = touchStartX / NAV_BTN_WIDTH;
        if (btnIdx >= 0 && btnIdx < MODE_COUNT) {
          if (btnIdx != currentMode) {
            currentMode = (UIMode)btnIdx;
            modeChanged = true;
            for (int i = 0; i < MODE_COUNT; i++) modeDrawn[i] = false;
            prevH = prevM = prevS = -1;
          }
          drawNavBar();
        }
      } else if (currentMode == MODE_RADIO) {
        handleRadioTap(touchStartX, touchStartY);
      } else if (currentMode == MODE_CLOCK && touchStartY < 30 && touchStartX < 100) {
        font16.drawString(100, 100, "WiFi配网模式", TFT_YELLOW, COLOR_BG);
        font16.drawString(80, 130, "手机连接: ESP32-Radio", TFT_WHITE, COLOR_BG);
        font16.drawString(100, 150, "密码: 12345678", TFT_WHITE, COLOR_BG);
        font16.drawString(80, 170, "然后访问 192.168.4.1", TFT_CYAN, COLOR_BG);
        WiFiManager wm;
        wm.setConfigPortalTimeout(180);
        wm.startConfigPortal("ESP32-Radio", "12345678");
        if (WiFi.status() == WL_CONNECTED) { wifiConnected = true; configTime(gmtOffsetSec, 0, ntpServer); }
        modeChanged = true; prevH = prevM = prevS = -1;
      }
      delay(150);
    }
    touchDidScroll = false;
    return;
  }

  if (isDown && currentMode == MODE_RADIO) {
    int dy = ty - touchStartY;
    if (abs(dy) > 10 && !touchDidScroll) touchDidScroll = true;
    if (touchDidScroll) {
      int newScroll = touchScrollStartScrollTop + dy / 3;
      newScroll = constrain(newScroll, 0, max(0, curRegionCount - RADIO_STATION_ROWS));
      if (newScroll != listScrollTop) { listScrollTop = newScroll; listScrollDirty = true; }
    }
    touchLastX = tx; touchLastY = ty;
  }
}

// ============================================================
// modeClock / modeCalendar / modeWeather
// ============================================================
void modeClock() {
  time_t now = time(nullptr);
  struct tm tm_info;
  localtime_r(&now, &tm_info);
  int h = tm_info.tm_hour, m = tm_info.tm_min, s = tm_info.tm_sec;

  if (!modeDrawn[MODE_CLOCK] || modeChanged) {
    tft.fillRect(0, 0, 480, NAV_BAR_Y, COLOR_BG);
    modeDrawn[MODE_CLOCK] = true;
  }

  if (h != prevH || m != prevM || s != prevS) {
    prevH = h; prevM = m; prevS = s;
    char timeStr[10];
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", h, m, s);
    tft.fillRect(100, 120, 280, 60, COLOR_BG);
    font24.drawString(140, 130, timeStr, TFT_YELLOW, COLOR_BG);
  }
}

void modeCalendar() {
  if (!modeDrawn[MODE_CALENDAR] || modeChanged) {
    tft.fillRect(0, 0, 480, NAV_BAR_Y, COLOR_BG);
    font24.drawString(160, 100, "万年历", TFT_CYAN, COLOR_BG);
    modeDrawn[MODE_CALENDAR] = true;
  }
}

void drawWeatherScreen() {
  tft.fillRect(0, 0, 480, NAV_BAR_Y, COLOR_BG);
  font16.drawString(10, 5, "天气", COLOR_ACCENT, COLOR_BG);
  font24.drawString(100, 100, "待接入和风天气API", TFT_YELLOW, COLOR_BG);
}

void modeWeather() {
  if (!modeDrawn[MODE_WEATHER] || modeChanged) {
    drawWeatherScreen();
    modeDrawn[MODE_WEATHER] = true;
  }
}

// ============================================================
// 收音机数据加载
// ============================================================
bool loadRadioDB() {
  File f = LittleFS.open("/stations.bin", "r");
  if (!f) { Serial.println(F("stations.bin not found!")); return false; }
  uint8_t magic[9]; f.read(magic, 8); magic[8] = 0;
  if (strcmp((char*)magic, "ESP32RDB") != 0 || f.read() != 2) { Serial.println(F("bad format")); f.close(); return false; }
  uint16_t numRegions = f.read() | (f.read() << 8);
  uint32_t numStations = f.read() | (f.read() << 8) | (f.read() << 16) | (f.read() << 24);
  uint32_t regionIdxOffset = f.read() | (f.read() << 8) | (f.read() << 16) | (f.read() << 24);
  uint32_t stationRecordsOffset = f.read() | (f.read() << 8) | (f.read() << 16) | (f.read() << 24);
  f.read();
  Serial.printf("stations.bin: %d regions, %lu stations\n", numRegions, numStations);
  f.seek(regionIdxOffset);
  regionEntries = new RegionEntry[numRegions];
  for (uint16_t i = 0; i < numRegions; i++) {
    f.read((uint8_t*)regionEntries[i].name, 16); regionEntries[i].name[16] = 0;
    regionEntries[i].first_offset = f.read() | (f.read() << 8) | (f.read() << 16) | (f.read() << 24);
    regionEntries[i].count = f.read() | (f.read() << 8);
    f.seek(f.position() + 6);
  }
  regionCount = numRegions; curRegionIdx = 0;
  loadRegionStations(curRegionIdx);
  radioInited = true;
  f.close(); return true;
}

void loadRegionStations(int regionIdx) {
  if (regionIdx < 0 || regionIdx >= regionCount) return;
  File f = LittleFS.open("/stations.bin", "r");
  if (!f) return;
  uint32_t offset = regionEntries[regionIdx].first_offset;
  uint16_t count = regionEntries[regionIdx].count;
  if (curRegionStations) { delete[] curRegionStations; curRegionStations = nullptr; }
  curRegionStations = new StationView[count]; curRegionCount = count;
  f.seek(offset);
  for (int i = 0; i < count; i++) {
    uint32_t recordStart = f.position();
    uint8_t nameLen = f.read(); uint8_t rawName[61]; f.read(rawName, nameLen); rawName[nameLen] = 0;
    if (nameLen <= 31) memcpy(curRegionStations[i].name, rawName, nameLen + 1);
    else { memcpy(curRegionStations[i].name, rawName, 28); curRegionStations[i].name[28]=curRegionStations[i].name[29]=curRegionStations[i].name[30]='.'; curRegionStations[i].name[31]=0; }
    uint16_t urlLen = f.read() | (f.read() << 8); f.seek(f.position() + urlLen);
    curRegionStations[i].region_idx = f.read();
    uint8_t catLen = f.read(); f.seek(f.position() + catLen);
    uint8_t freqLen = f.read(); uint8_t rawFreq[21]; f.read(rawFreq, freqLen); rawFreq[freqLen] = 0;
    if (freqLen <= 12) memcpy(curRegionStations[i].freq, rawFreq, freqLen + 1);
    else { memcpy(curRegionStations[i].freq, rawFreq, 12); curRegionStations[i].freq[12] = 0; }
    curRegionStations[i].r = f.read(); curRegionStations[i].g = f.read(); curRegionStations[i].b = f.read();
    curRegionStations[i].bin_offset = recordStart;
  }
  f.close();
}

String readStationURL(uint32_t binOffset) {
  File f = LittleFS.open("/stations.bin", "r");
  if (!f) return "";
  f.seek(binOffset);
  uint8_t nameLen = f.read(); f.seek(f.position() + nameLen);
  uint16_t urlLen = f.read() | (f.read() << 8);
  uint8_t urlBuf[257]; f.read(urlBuf, min(urlLen, (uint16_t)256));
  urlBuf[min(urlLen, (uint16_t)256)] = 0; f.close();
  return String((char*)urlBuf);
}

void playStationAt(int listIdx) {
  if (listIdx < 0 || listIdx >= curRegionCount) return;
  if (currentPlayingIdx == listIdx && radioPlaying) return;
  String url = readStationURL(curRegionStations[listIdx].bin_offset);
  if (url.length() == 0) return;
  if (radioPlaying) audio.stopSong();
  Serial.printf("Playing [%d]: %s\n", listIdx, curRegionStations[listIdx].name);
  audio.connecttohost(url.c_str());
  radioPlaying = true; currentPlayingIdx = listIdx;
  digitalWrite(STATUS_LED_PIN, HIGH);
}

void stopRadio() { audio.stopSong(); radioPlaying = false; digitalWrite(STATUS_LED_PIN, LOW); }

// ============================================================
// 收藏
// ============================================================
void loadFavorites() {
  File f = LittleFS.open("/favorites.bin", "r");
  if (!f) { favoriteCount = 0; return; }
  favoriteCount = 0;
  while (f.available() && favoriteCount < MAX_FAVORITES) {
    uint32_t off = f.read() | (f.read() << 8) | (f.read() << 16) | (f.read() << 24);
    favorites[favoriteCount++] = off;
  }
  f.close();
  Serial.printf("Loaded %d favorites\n", favoriteCount);
}

void saveFavorites() {
  File f = LittleFS.open("/favorites.bin", "w");
  if (!f) return;
  for (int i = 0; i < favoriteCount; i++) {
    uint32_t off = favorites[i];
    f.write(off & 0xFF); f.write((off >> 8) & 0xFF); f.write((off >> 16) & 0xFF); f.write((off >> 24) & 0xFF);
  }
  f.close();
}

bool isFavorited(uint32_t binOffset) {
  for (int i = 0; i < favoriteCount; i++) if (favorites[i] == binOffset) return true;
  return false;
}

void toggleFavorite(uint32_t binOffset) {
  for (int i = 0; i < favoriteCount; i++) {
    if (favorites[i] == binOffset) {
      for (int j = i; j < favoriteCount - 1; j++) favorites[j] = favorites[j+1];
      favoriteCount--; saveFavorites(); return;
    }
  }
  if (favoriteCount < MAX_FAVORITES) { favorites[favoriteCount++] = binOffset; saveFavorites(); }
}

// ============================================================
// Audio 回调
// ============================================================
void audio_info(const char* info) { Serial.printf("info: %s\n", info); }
void audio_showstation(const char* info) { Serial.printf("station: %s\n", info); }
void audio_showstreamtitle(const char* info) { Serial.printf("stream: %s\n", info); }

// ============================================================
// setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("ESP32-S3 Smart Radio Starting...");
  LittleFS.begin();
  initTFT();
  initTouch();
  xfont_open_font(&font16, "/font_simhei_16.font");
  xfont_open_font(&font24, "/font_simhei_24.font");
  Serial.println("Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin("HUAWEI-2.4G-A702", "a1b2c3d4e5");
  unsigned long ws = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - ws < 15000) { delay(500); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("\nWiFi connected: %s\n", WiFi.localIP().toString().c_str());
    configTime(gmtOffsetSec, 0, ntpServer); delay(1000);
  }
  Serial.println("Loading radio DB...");
  loadRadioDB();
  loadFavorites();
  audio.setPinout(I2S_BCK_PIN, I2S_LRC_PIN, I2S_DOUT_PIN);
  audio.setBufsize(16384, 65536);
  audio.setConnectionTimeout(200, 2000);
  Serial.println("Setup done.");
  modeChanged = true;
}

void loop() {
  handleTouch();
  switch (currentMode) {
    case MODE_CLOCK: modeClock(); break;
    case MODE_CALENDAR: modeCalendar(); break;
    case MODE_RADIO: modeRadio(); break;
    case MODE_WEATHER: modeWeather(); break;
  }
  drawNavBar();
  if (modeChanged) modeChanged = false;
}
'''

result = header + new_funcs + after

# 去重 drawRadioScrollbar 声明
lines = result.split('\n')
seen = False
clean = []
for line in lines:
    if 'void drawRadioScrollbar();' in line:
        if seen: continue
        seen = True
    clean.append(line)

out = '\n'.join(clean)
open(r'd:\ESP32-S3\src\main.cpp', 'w', encoding='utf-8').write(out)
print(f'Written: {len(out)} chars')
