/*
 * XFont 中文字库实现 - ESP32 网络收音机适配版
 * 基于 TFT_eSPI 驱动
 */

#include "xfont.h"
#include "esp_heap_caps.h"   // heap_caps_malloc_psram / heap_free_psram
#include <string.h>          // memcpy (PSRAM alpha 中转)

// ============================================================
// 构造与析构
// ============================================================

XFont::XFont(TFT_eSPI* tft)
    : _tft(tft)
    , _isInit(false)
    , _isAA(false)
    , _totalFontCount(0)
    , _fontSize(0)
    , _binType(64)
    , _fontPage(0)
    , _unicodeBeginIdx(0)
    , _screenWidth(0)
    , _screenHeight(0)
{
    if (_tft) {
        _screenWidth = _tft->width();
        _screenHeight = _tft->height();
    }
}

XFont::~XFont()
{
    clear();
}

void XFont::clear()
{
    if (_fontFile) {
        _fontFile.close();
    }
    _allUnicodes = "";
    _isInit = false;
    _isAA = false;
    if (_aaBlob) { heap_caps_free(_aaBlob); _aaBlob = nullptr; }
    if (_aaUni)   { heap_caps_free(_aaUni);   _aaUni = nullptr; }
    if (_aaAdv)   { heap_caps_free(_aaAdv);   _aaAdv = nullptr; }
    _aaPixels = nullptr;
}

// ============================================================
// 初始化字库
// ============================================================

bool XFont::begin(const String& fontPath)
{
    if (_isInit) {
        return true;
    }

    _fontFilePath = fontPath;

    // 初始化 LittleFS
    if (!LittleFS.begin(false)) {
        Serial.println(F("[XFont] LittleFS 挂载失败"));
        return false;
    }

    // 检查字库文件是否存在
    if (!LittleFS.exists(fontPath)) {
        Serial.printf("[XFont] 字库文件不存在: %s\n", fontPath.c_str());
        return false;
    }

    // 打开字库文件
    _fontFile = LittleFS.open(fontPath, "r");
    if (!_fontFile) {
        Serial.println(F("[XFont] 无法打开字库文件"));
        return false;
    }

    // 新版 AA (8-bit 灰度) 格式: magic "XAAX" (gen_aa_font.py 生成)
    {
        uint8_t magic[4];
        _fontFile.seek(0);
        int mn = _fontFile.read(magic, 4);
        if (mn == 4 && magic[0]=='X' && magic[1]=='A' && magic[2]=='A' && magic[3]=='X') {
            _fontFile.close();
            return loadAA(fontPath);
        }
        _fontFile.seek(0);
    }

    // 读取文件头信息
    uint8_t bufTotal[6];
    uint8_t bufSize[2];
    uint8_t bufBin[2];

    _fontFile.read(bufTotal, 6);    // 总字数 (6位十六进制字符串)
    _fontFile.read(bufSize, 2);     // 字号 (2位数字字符串)
    _fontFile.read(bufBin, 2);      // 编码类型 (2位数字字符串)

    String sTotal = charsToString(bufTotal, 6);
    String sSize = charsToString(bufSize, 2);
    String sBin = charsToString(bufBin, 2);

    _totalFontCount = strtol(sTotal.c_str(), NULL, 16);
    _fontSize = sSize.toInt();
    _binType = sBin.toInt();
    _fontPage = calcFontPage(_fontSize, _binType);

    // 根据文件实际大小修正 _fontPage
    // 16号字库有分隔符(page=hexCount+1=44), 24号字库无分隔符(page=hexCount=96)
    int headerSize = 10;
    int indexSize = _totalFontCount * 5;
    int pixelDataSize = _fontFile.size() - headerSize - indexSize;
    if (_totalFontCount > 0) {
        int actualPage = pixelDataSize / _totalFontCount;
        if (actualPage != _fontPage) {
            Serial.printf("[XFont] 修正fontPage: 计算=%d, 实际=%d\n", _fontPage, actualPage);
            _fontPage = actualPage;
        }
    }

    Serial.printf("[XFont] 字库信息: 总字数=%d, 字号=%d, 编码=%dbase, 每字%d字节\n",
                  _totalFontCount, _fontSize, _binType, _fontPage);

    // 读取 Unicode 索引
    int unicodeLen = _totalFontCount * 5;  // 每个字符占5字节 (uXXXX)

#ifdef XFONT_SAVE_MEMORY
    Serial.println(F("[XFont] 节省内存模式: 不预加载 Unicode 索引"));
#else
    // 一次性读取所有 Unicode 索引（ESP32-S3 内存充足）
    _allUnicodes.reserve(unicodeLen);
    int remaining = unicodeLen;
    const int readSize = 1024;
    uint8_t* buf = (uint8_t*)malloc(readSize);

    if (buf) {
        while (remaining > 0) {
            int toRead = (remaining < readSize) ? remaining : readSize;
            _fontFile.read(buf, toRead);
            _allUnicodes += charsToString(buf, toRead);
            remaining -= toRead;
        }
        free(buf);
        Serial.printf("[XFont] Unicode 索引加载完成, 共 %d 字符\n", _allUnicodes.length() / 5);
    } else {
        Serial.println(F("[XFont] 警告: 内存不足, 无法加载索引"));
    }
#endif

    // 像素数据起始位置
    _unicodeBeginIdx = 6 + 2 + 2 + unicodeLen;

    _fontFile.close();
    _isInit = true;

    Serial.printf("[XFont] 字库初始化完成: %s\n", fontPath.c_str());
    return true;
}

bool XFont::reInit(const String& fontPath)
{
    clear();
    return begin(fontPath);
}

// ============================================================
// UTF-8 转 Unicode
// ============================================================

String XFont::utf8ToUnicode(const String& utf8Str)
{
    String result = "";
    result.reserve(utf8Str.length() * 2);

    for (uint i = 0; i < utf8Str.length(); ) {
        unsigned char c = utf8Str[i];

        if (c < 0x80) {
            // ASCII 字符 (1字节)
            char hex[5];
            snprintf(hex, sizeof(hex), "%04x", c);
            result += hex;
            i++;
        } else if (c < 0xE0) {
            // 2字节 UTF-8
            uint16_t code = ((c & 0x1F) << 6) | (utf8Str[i + 1] & 0x3F);
            char hex[5];
            snprintf(hex, sizeof(hex), "%04x", code);
            result += hex;
            i += 2;
        } else if (c < 0xF0) {
            // 3字节 UTF-8 (常用汉字)
            uint16_t code = ((c & 0x0F) << 12) |
                            ((utf8Str[i + 1] & 0x3F) << 6) |
                            (utf8Str[i + 2] & 0x3F);
            char hex[5];
            snprintf(hex, sizeof(hex), "%04x", code);
            result += hex;
            i += 3;
        } else {
            // 4字节 UTF-8 (罕用字, 暂不支持)
            i += 4;
        }
    }

    return result;
}

// ============================================================
// 从字库文件获取字符编码
// ============================================================

String XFont::getCodeFromFile(const String& unicodeStr)
{
    String result = "";

    if (!_isInit) return result;

    _fontFile = LittleFS.open(_fontFilePath, "r");
    if (!_fontFile) return result;

    int charCount = unicodeStr.length() / 4;
    uint8_t* pixBuf = (uint8_t*)malloc(_fontPage);

    if (!pixBuf) {
        _fontFile.close();
        return result;
    }

    const char* allUnicodesCStr = _allUnicodes.c_str();

    for (int i = 0; i < charCount; i++) {
        String unicodeChar = unicodeStr.substring(i * 4, (i + 1) * 4);
        String searchStr = "u" + unicodeChar;

        int idx = -1;
        int charIdx = 0;

        // 诊断: 第一次查找时打印
        static int debugCount = 0;
        if (debugCount < 5) {
            debugCount++;
            Serial.printf("[XFont debug] Looking for: '%s' in %d chars\n",
                searchStr.c_str(), _totalFontCount);
            // 打印前20个字符的索引
            Serial.printf("[XFont debug] Index[0..19]: ", _allUnicodes.c_str());
            for (int j = 0; j < 20 && j < (int)_allUnicodes.length()/5; j++) {
                char tmp[6] = {0};
                memcpy(tmp, _allUnicodes.c_str() + j*5, 5);
                Serial.printf("%s ", tmp);
            }
            Serial.println();
        }

#ifndef XFONT_SAVE_MEMORY
        // 内存查找（快）
        char* found = strstr(allUnicodesCStr, searchStr.c_str());
        if (found) {
            idx = found - allUnicodesCStr;
            charIdx = idx / 5;
        }
#else
        // 文件查找（慢，但省内存）
        _fontFile.seek(10);  // 跳过文件头
        int remaining = _totalFontCount * 5;
        const int readSize = 512;
        uint8_t* buf = (uint8_t*)malloc(readSize);
        int loopCount = 0;

        if (buf) {
            while (remaining > 0) {
                int toRead = (remaining < readSize) ? remaining : readSize;
                _fontFile.read(buf, toRead);
                String chunk = charsToString(buf, toRead);
                int pos = chunk.indexOf(searchStr);
                if (pos >= 0) {
                    idx = pos + loopCount * readSize;
                    charIdx = idx / 5;
                    free(buf);
                    break;
                }
                remaining -= toRead;
                loopCount++;
            }
            if (remaining <= 0) free(buf);
        }
#endif

        if (idx >= 0) {
            // 找到字符，读取像素数据
            int pixOffset = _unicodeBeginIdx + charIdx * _fontPage;
            _fontFile.seek(pixOffset);
            _fontFile.read(pixBuf, _fontPage);
            result += charsToString(pixBuf, _fontPage);
        } else {
            // 未找到字符，添加空白
            for (int j = 0; j < _fontPage; j++) {
                result += "0";
            }
        }
    }

    free(pixBuf);
    _fontFile.close();
    return result;
}

// ============================================================
// 64进制编码转二进制像素字符串
// ============================================================

String XFont::hexToPixelData(const String& hexStr)
{
    String result = "";
    int totalPixels = _fontSize * _fontSize;
    result.reserve(totalPixels);

    for (uint i = 0; i < hexStr.length(); i++) {
        char c = hexStr[i];
        int val = 0;

        // 查找字符在64进制表中的位置
        const char* p = strchr(_s64, c);
        if (p) {
            val = p - _s64;
        }

        // 6位二进制
        for (int k = 5; k >= 0; k--) {
            result += (val & (1 << k)) ? '1' : '0';
        }
    }

    // 截断到实际像素数
    if ((int)result.length() > totalPixels) {
        result = result.substring(0, totalPixels);
    }

    return result;
}

// ============================================================
// 计算每字占用字节数
// ============================================================

int XFont::calcFontPage(int fontSize, int binType)
{
    int total = fontSize * fontSize;
    int bitsPerChar = (binType == 32) ? 5 : 6;  // 32进制=5bit, 64进制=6bit
    int hexCount = (total + bitsPerChar - 1) / bitsPerChar;  // 向上取整
    // 16号字库有分隔符(hexCount+1=44), 24号字库无分隔符(hexCount=96)
    // 通过文件实际大小判断: 在begin()中修正
    return hexCount;
}

// ============================================================
// 工具函数
// ============================================================

bool XFont::isAscii(unsigned char c)
{
    return c <= 127;
}

String XFont::charsToString(uint8_t* buf, int len)
{
    char* tmp = (char*)malloc(len + 1);
    if (!tmp) return "";
    memcpy(tmp, buf, len);
    tmp[len] = '\0';
    String s = tmp;
    free(tmp);
    return s;
}

// ============================================================
// 绘制单个字符
// ============================================================

void XFont::drawGlyph(int x, int y, const String& pixelData, uint16_t color, bool isAsciiChar)
{
    int drawWidth = isAsciiChar ? (_fontSize / 2) : _fontSize;
    int totalLen = pixelData.length();

    _tft->startWrite();

    for (int i = 0; i < totalLen; i++) {
        if (pixelData[i] == '1') {
            int px = (i % _fontSize) + x;
            int py = (i / _fontSize) + y;

            if (isAsciiChar && (i % _fontSize) >= _fontSize / 2) {
                continue;  // ASCII 只显示左半部分
            }

            _tft->drawPixel(px, py, color);
        }
    }

    _tft->endWrite();
}

void XFont::drawGlyph(int x, int y, const String& pixelData, uint16_t color, uint16_t bgColor, bool isAsciiChar)
{
    int drawWidth = isAsciiChar ? (_fontSize / 2) : _fontSize;
    int totalLen = pixelData.length();

    _tft->startWrite();

    for (int i = 0; i < totalLen; i++) {
        int px = (i % _fontSize) + x;
        int py = (i / _fontSize) + y;

        if (isAsciiChar && (i % _fontSize) >= _fontSize / 2) {
            continue;
        }

        if (pixelData[i] == '1') {
            _tft->drawPixel(px, py, color);
        } else {
            _tft->drawPixel(px, py, bgColor);
        }
    }

    _tft->endWrite();
}

// ============================================================
// 绘制字符串 - 公开 API
// ============================================================

void XFont::drawString(int x, int y, const String& str, uint16_t color)
{
    if (!_isInit || !_tft) return;
    if (_isAA) { drawStringFastAA(x, y, str, color, 0x0000); return; }

    String unicodeStr = utf8ToUnicode(str);
    int charCount = unicodeStr.length() / 4;

    int curX = x;
    int curY = y;
    int sep = 1;  // 字间距

    for (int i = 0; i < charCount; i++) {
        String unicodeChar = unicodeStr.substring(i * 4, (i + 1) * 4);
        String codeData = getCodeFromFile(unicodeChar);
        String pixelData = hexToPixelData(codeData);

        // 判断是否为 ASCII
        uint16_t codeVal = strtol(unicodeChar.c_str(), NULL, 16);
        bool ascii = (codeVal <= 127);

        // 处理换行
        if (codeVal == '\n' || codeVal == '\r') {
            curX = x;
            curY += _fontSize + sep;
            continue;
        }

        // 自动换行
        int charW = ascii ? (_fontSize / 2 + sep) : (_fontSize + sep);
        if (curX + charW > _screenWidth + x) {
            curX = x;
            curY += _fontSize + sep;
        }

        // 透明背景：只绘制前景像素
        drawGlyph(curX, curY, pixelData, color, ascii);
        curX += ascii ? (_fontSize / 2 + sep) : (_fontSize + sep);
    }
}

void XFont::drawString(int x, int y, const String& str, uint16_t color, uint16_t bgColor)
{
    if (!_isInit || !_tft) return;
    if (_isAA) { drawStringFastAA(x, y, str, color, bgColor); return; }

    String unicodeStr = utf8ToUnicode(str);
    int charCount = unicodeStr.length() / 4;

    int curX = x;
    int curY = y;
    int sep = 1;  // 字间距

    for (int i = 0; i < charCount; i++) {
        String unicodeChar = unicodeStr.substring(i * 4, (i + 1) * 4);
        String codeData = getCodeFromFile(unicodeChar);
        String pixelData = hexToPixelData(codeData);

        // 判断是否为 ASCII
        uint16_t codeVal = strtol(unicodeChar.c_str(), NULL, 16);
        bool ascii = (codeVal <= 127);

        // 处理换行
        if (codeVal == '\n' || codeVal == '\r') {
            curX = x;
            curY += _fontSize + sep;
            continue;
        }

        // 自动换行
        int charW = ascii ? (_fontSize / 2 + sep) : (_fontSize + sep);
        if (curX + charW > _screenWidth + x) {
            curX = x;
            curY += _fontSize + sep;
        }

        // 带背景色：每个像素都绘制
        drawGlyph(curX, curY, pixelData, color, bgColor, ascii);
        curX += ascii ? (_fontSize / 2 + sep) : (_fontSize + sep);
    }
}

void XFont::drawStringFast(int x, int y, const String& str, uint16_t color)
{
    if (!_isInit || !_tft) return;
    if (_isAA) { drawStringFastAA(x, y, str, color, 0x0000); return; }

    String unicodeStr = utf8ToUnicode(str);
    String allCodeData = getCodeFromFile(unicodeStr);  // 一次性读取所有字符

    int charCount = unicodeStr.length() / 4;
    int curX = x;
    int curY = y;
    int sep = 1;

    for (int i = 0; i < charCount; i++) {
        String unicodeChar = unicodeStr.substring(i * 4, (i + 1) * 4);
        String codeData = allCodeData.substring(i * _fontPage, (i + 1) * _fontPage);
        String pixelData = hexToPixelData(codeData);

        uint16_t codeVal = strtol(unicodeChar.c_str(), NULL, 16);
        bool ascii = (codeVal <= 127);

        if (codeVal == '\n' || codeVal == '\r') {
            curX = x;
            curY += _fontSize + sep;
            continue;
        }

        int charW = ascii ? (_fontSize / 2 + sep) : (_fontSize + sep);
        if (curX + charW > _screenWidth + x) {
            curX = x;
            curY += _fontSize + sep;
        }

        // 透明背景：只绘制前景像素
        drawGlyph(curX, curY, pixelData, color, ascii);
        curX += ascii ? (_fontSize / 2 + sep) : (_fontSize + sep);
    }
}

void XFont::drawStringFast(int x, int y, const String& str, uint16_t color, uint16_t bgColor)
{
    if (!_isInit || !_tft) return;
    if (_isAA) { drawStringFastAA(x, y, str, color, bgColor); return; }

    String unicodeStr = utf8ToUnicode(str);
    String allCodeData = getCodeFromFile(unicodeStr);  // 一次性读取所有字符

    int charCount = unicodeStr.length() / 4;
    int curX = x;
    int curY = y;
    int sep = 1;

    for (int i = 0; i < charCount; i++) {
        String unicodeChar = unicodeStr.substring(i * 4, (i + 1) * 4);
        String codeData = allCodeData.substring(i * _fontPage, (i + 1) * _fontPage);
        String pixelData = hexToPixelData(codeData);

        uint16_t codeVal = strtol(unicodeChar.c_str(), NULL, 16);
        bool ascii = (codeVal <= 127);

        if (codeVal == '\n' || codeVal == '\r') {
            curX = x;
            curY += _fontSize + sep;
            continue;
        }

        int charW = ascii ? (_fontSize / 2 + sep) : (_fontSize + sep);
        if (curX + charW > _screenWidth + x) {
            curX = x;
            curY += _fontSize + sep;
        }

        drawGlyph(curX, curY, pixelData, color, bgColor, ascii);
        curX += ascii ? (_fontSize / 2 + sep) : (_fontSize + sep);
    }
}

// ============================================================
// 计算字符串宽度
// ============================================================

int XFont::textWidth(const String& str)
{
    if (!_isInit || _fontSize <= 0) return 0;
    if (_isAA) return textWidthAA(str);

    String unicodeStr = utf8ToUnicode(str);
    int charCount = unicodeStr.length() / 4;
    int width = 0;
    int sep = 1;

    for (int i = 0; i < charCount; i++) {
        String unicodeChar = unicodeStr.substring(i * 4, (i + 1) * 4);
        uint16_t codeVal = strtol(unicodeChar.c_str(), NULL, 16);

        if (codeVal == '\n' || codeVal == '\r') continue;

        if (codeVal <= 127) {
            width += _fontSize / 2 + sep;
        } else {
            width += _fontSize + sep;
        }
    }

    return width;
}

// ============================================================
// 获取像素数据字符串
// ============================================================

String XFont::getPixelData(const String& str)
{
    if (!_isInit) return "";

    String unicodeStr = utf8ToUnicode(str);
    String codeData = getCodeFromFile(unicodeStr);
    String result = "";

    int charCount = unicodeStr.length() / 4;
    for (int i = 0; i < charCount; i++) {
        String charCode = codeData.substring(i * _fontPage, (i + 1) * _fontPage);
        result += hexToPixelData(charCode);
    }

    return result;
}

// ============================================================
// AA 模式 (8-bit 灰度, 逐字 advance, 抗锯齿)
// 文件格式: magic "XAAX"(4) | ver(1) | (1) | n(2 BE) |
//           每字 [unicode(2 BE) + advance(1) + 16x16 alpha(256)]
// ============================================================

bool XFont::loadAA(const String& fontPath)
{
    File f = LittleFS.open(fontPath, "r");
    if (!f) { Serial.printf("[XFont] loadAA: 打开失败 %s\n", fontPath.c_str()); return false; }
    uint32_t size = f.size();
    if (size < 8) { f.close(); return false; }

    // 整个文件载入 PSRAM
    uint8_t* blob = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blob) { Serial.printf("[XFont] loadAA: PSRAM alloc %u B 失败\n", size); f.close(); return false; }
    f.read(blob, size);
    f.close();

    // 头/字段均为**大端** (与 gen_aa_font.py / gen_aa_font32.py / make_preview.py 一致: unicode(2,BE), n(2,BE))
    uint16_t count = (uint16_t)((blob[6] << 8) | blob[7]);
    // ver 决定单元格: ver1=16x16(256B), ver2=32x32(1024B); 步进 = unicode(2)+adv(1)+alpha
    _aaSize = (blob[4] == 3) ? 64 : (blob[4] == 4) ? 24 : (blob[4] == 2) ? 32 : 16;   // ver1=16 ver2=32 ver3=64 ver4=24(1.5x超采样16px)
    const uint32_t perGlyph = (uint32_t)(3 + _aaSize * _aaSize);
    if (size < 8u + (uint32_t)count * perGlyph) {
        Serial.printf("[XFont] loadAA: size %u < 8+%u*%u 不符\n", size, count, perGlyph);
        heap_caps_free(blob); return false; }

    _isAA = true;
    _fontSize = _aaSize;
    _totalFontCount = count;
    _aaBlob = blob;
    _aaPixels = blob + 8;

    // 拆出 unicode / advance 数组 (PSRAM)
    _aaUni = (uint16_t*)heap_caps_malloc(count * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    _aaAdv = (uint8_t*)heap_caps_malloc(count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_aaUni || !_aaAdv) { clear(); return false; }
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t* g = _aaPixels + i * perGlyph;
        _aaUni[i] = (uint16_t)((g[0] << 8) | g[1]);   // unicode 大端
        _aaAdv[i] = g[2];
    }

    _isInit = true;
    Serial.printf("[XFont] AA 字库已加载: %s (%d 字, %dpx, %u KB)\n",
                  fontPath.c_str(), count, _aaSize, size / 1024);
    return true;
}

int XFont::findAA(uint16_t code) const
{
    // 二分查找 (unicode 升序)
    int lo = 0, hi = _totalFontCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (_aaUni[mid] == code) return mid;
        else if (_aaUni[mid] < code) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

int XFont::textWidthAA(const String& str) const
{
    int w = 0;
    for (uint i = 0; i < str.length(); i++) {
        unsigned char c = str[i];
        if (c < 0x80) {
            int idx = findAA(c);
            w += (idx >= 0) ? _aaAdv[idx] : 8;
        } else if (c < 0xE0) {
            uint16_t code = ((c & 0x1F) << 6) | (str[i + 1] & 0x3F); i++;
            int idx = findAA(code);
            w += (idx >= 0) ? _aaAdv[idx] : 16;
        } else if (c < 0xF0) {
            uint16_t code = ((c & 0x0F) << 12) | ((str[i + 1] & 0x3F) << 6) | (str[i + 2] & 0x3F); i += 2;
            int idx = findAA(code);
            w += (idx >= 0) ? _aaAdv[idx] : 16;
        } else {
            i += 3;
        }
    }
    return w;
}

void XFont::compositeGlyph(int x, int y, int idx, uint16_t fg, uint16_t bg) const
{
    const int S = _aaSize;                          // 原生单元格边长 (ver4=24 超采样 / ver1=16 / ver3=64)
    const uint8_t* a = _aaPixels + idx * (3 + S * S) + 3;   // 跳过 unicode(2)+adv(1)
    // ⚠️ 内部 RAM 中转: SxS 原生灰度面积平均降采样到 16x16 再逐通道 565 合成.
    // 避免 ESP32-S3 八线 PSRAM DMA 突发读取错乱 (历史 bug 2026-09-22). 与 drawScaledChar 同款.
    static uint8_t alphaBuf[64 * 64];               // S<=64 -> 4096B 内部 RAM
    memcpy(alphaBuf, a, (size_t)S * S);
    int fr = fg >> 11, fb = fg & 0x1F, fm = (fg >> 5) & 0x3F;
    int br = bg >> 11, bb = bg & 0x1F, bm = (bg >> 5) & 0x3F;
    for (int oy = 0; oy < 16; oy++) {
        int sy0 = (oy * S) / 16, sy1 = ((oy + 1) * S) / 16;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int ox = 0; ox < 16; ox++) {
            int sx0 = (ox * S) / 16, sx1 = ((ox + 1) * S) / 16;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            long sum = 0; int n = 0;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++) { sum += alphaBuf[sy * S + sx]; n++; }
            int m = (int)(sum / n);                 // 与 blitDialNum 同款: out = (bg*(255-m) + fg*m) / 255
            _buf[oy * 16 + ox] = (((br * (255 - m) + fr * m) / 255) << 11) |
                                  (((bm * (255 - m) + fm * m) / 255) << 5)  |
                                  ((bb * (255 - m) + fb * m) / 255);
        }
    }
    _tft->pushImage(x, y, 16, 16, _buf);
}

// 把一个 16x16 AA 灰度字形"缩小"绘制 (面积平均 + 逐通道 565 合成), 用于日历角标等
// 需要比 16px 更小的中文场景。dst=目标边长 (如 12 => 12x12); 1px 源块映射到
// ceil(16/dst) 个目标像素 (12 => 2x2, 平均后约 12.8px 视觉字号, 带抗锯齿)。
// 合成公式与 compositeGlyph 一致: out = (bg*(255-m) + fg*m)/255。
void XFont::drawScaledChar(int x, int y, uint16_t ch, int dst, uint16_t fg, uint16_t bg) const
{
    if (!dst) dst = 1;
    if (dst > 16) dst = 16;
    int idx = findAA(ch);
    if (idx < 0) return;
    const int S = _aaSize;                          // 原生单元格边长 (ver4=24 超采样)
    const uint8_t* a = _aaPixels + idx * (3 + S * S) + 3;   // 跳过 unicode(2)+adv(1)
    // ⚠️ 内部 RAM 中转: SxS 原生灰度面积平均缩放到 dst x dst 再逐通道 565 合成 (与 compositeGlyph 同款)
    static uint8_t alphaBuf[64 * 64];
    memcpy(alphaBuf, a, (size_t)S * S);
    int fr = fg >> 11, fb = fg & 0x1F, fm = (fg >> 5) & 0x3F;
    int br = bg >> 11, bb = bg & 0x1F, bm = (bg >> 5) & 0x3F;
    static uint16_t sbuf[256];
    for (int oy = 0; oy < dst; oy++) {
        for (int ox = 0; ox < dst; ox++) {
            int sy0 = (oy * S) / dst, sy1 = ((oy + 1) * S) / dst;   // 源行(y)
            int sx0 = (ox * S) / dst, sx1 = ((ox + 1) * S) / dst;   // 源列(x)
            if (sy1 <= sy0) sy1 = sy0 + 1;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            long sum = 0;
            int n = 0;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++) { sum += alphaBuf[sy * S + sx]; n++; }
            int m = (int)(sum / n);
            sbuf[oy * dst + ox] = (((br * (255 - m) + fr * m) / 255) << 11) |
                                  (((bm * (255 - m) + fm * m) / 255) << 5)  |
                                  ((bb * (255 - m) + fb * m) / 255);
        }
    }
    _tft->pushImage(x, y, dst, dst, sbuf);
}

// 放大绘制一个中文大标题字符: 源 AA 灰度 (_aaSize x _aaSize, 16 或 32) 上采样到 dst x dst (dst 可达 40)。
// 顶角最近邻取样 (sy=(oy*S)/dst, 与 preview big_glyph 逐像素一致) + 逐通道 565 合成 (out=(bg*(255-m)+fg*m)/255)。
// 用内部 RAM 行/块缓冲中转 (alpha<=1024B, dst<=40 -> 1600px), 避免八线 PSRAM 直读花屏 (硬件反模式 R3)。
// 仅用于黄历大字等比字库更大的中文; 16px 及以下仍走 drawScaledChar/compositeGlyph。
void XFont::drawBigChar(int x, int y, uint16_t ch, int dst, uint16_t fg, uint16_t bg) const
{
    if (!dst) dst = 1;
    if (dst > 40) dst = 40;
    int idx = findAA(ch);
    if (idx < 0) return;
    int S = _aaSize;                       // 字库原生字形边长 (16 或 32)
    const uint32_t perGlyph = (uint32_t)(3 + S * S);
    const uint8_t* a = _aaPixels + (uint32_t)idx * perGlyph + 3;   // 跳过 unicode(2)+adv(1)
    // ⚠️ 内部 RAM 中转 alpha (与 compositeGlyph 同款, 避免 PSRAM DMA 错乱)
    static uint8_t alphaBuf[64 * 64];   // ver3=64x64=4096B (内部 RAM)
    memcpy(alphaBuf, a, (size_t)S * S);
    int fr = fg >> 11, fb = fg & 0x1F, fm = (fg >> 5) & 0x3F;
    int br = bg >> 11, bb = bg & 0x1F, bm = (bg >> 5) & 0x3F;
    static uint16_t bigBuf[40 * 40];   // dst<=40 -> 1600 px 内部 RAM
    // 面积平均重采样 (与 preview big_glyph 同款): 每个目标像素平均其覆盖的源块 -> 边缘平滑 (非最近邻)
    for (int oy = 0; oy < dst; oy++) {
        int sy0 = (oy * S) / dst;
        int sy1 = ((oy + 1) * S) / dst;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy0 < 0) sy0 = 0; if (sy1 > S) sy1 = S;
        for (int ox = 0; ox < dst; ox++) {
            int sx0 = (ox * S) / dst;
            int sx1 = ((ox + 1) * S) / dst;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx0 < 0) sx0 = 0; if (sx1 > S) sx1 = S;
            int s = 0;
            for (int yy = sy0; yy < sy1; yy++)
                for (int xx = sx0; xx < sx1; xx++)
                    s += alphaBuf[yy * S + xx];
            int m = s / ((sy1 - sy0) * (sx1 - sx0));
            bigBuf[oy * dst + ox] = (((br * (255 - m) + fr * m) / 255) << 11) |
                                     (((bm * (255 - m) + fm * m) / 255) << 5)  |
                                     ((bb * (255 - m) + fb * m) / 255);
        }
    }
    _tft->pushImage(x, y, dst, dst, bigBuf);
}

void XFont::drawStringFastAA(int x, int y, const String& str, uint16_t fg, uint16_t bg) const
{
    int cx = x;
    for (uint i = 0; i < str.length(); i++) {
        unsigned char c = str[i];
        if (c < 0x80) {
            int idx = findAA(c);
            if (idx >= 0) { compositeGlyph(cx, y, idx, fg, bg); cx += _aaAdv[idx]; }
            else cx += 8;
        } else if (c < 0xE0) {
            uint16_t code = ((c & 0x1F) << 6) | (str[i + 1] & 0x3F); i++;
            int idx = findAA(code);
            if (idx >= 0) { compositeGlyph(cx, y, idx, fg, bg); cx += _aaAdv[idx]; }
            else cx += 16;
        } else if (c < 0xF0) {
            uint16_t code = ((c & 0x0F) << 12) | ((str[i + 1] & 0x3F) << 6) | (str[i + 2] & 0x3F); i += 2;
            int idx = findAA(code);
            if (idx >= 0) { compositeGlyph(cx, y, idx, fg, bg); cx += _aaAdv[idx]; }
            else cx += 16;
        } else {
            i += 3;
        }
    }
}
