/*
 * XFont 中文字库 - ESP32 网络收音机适配版
 * 基于 TFT_eSPI 驱动
 * 支持从 LittleFS 加载自定义中文字库
 *
 * 支持两种格式 (按文件头自动识别):
 *   A) 旧版 1-bit base-64 (font_simhei_24.font): 无抗锯齿, ASCII 固定半宽
 *   B) 新版 AA 8-bit 灰度 (font_simhei_16.font, magic "XAAX"): 抗锯齿,
 *      逐字 advance (与 make_preview.py 同源 msyhbd.ttc), pushImage 合成
 */

#ifndef _XFONT_H_
#define _XFONT_H_

#include <Arduino.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include <base64.h>

// 节省内存模式：不将所有 Unicode 索引加载到内存，每次查找都从文件读取
// #define XFONT_SAVE_MEMORY

class XFont
{
public:
    XFont(TFT_eSPI* tft);
    ~XFont();

    bool begin(const String& fontPath);
    bool reInit(const String& fontPath);
    void clear();
    bool isReady() { return _isInit; }
    int getFontSize() { return _fontSize; }

    // ===== 绘制字符串（推荐） =====
    void drawString(int x, int y, const String& str, uint16_t color);
    void drawString(int x, int y, const String& str, uint16_t color, uint16_t bgColor);
    void drawStringFast(int x, int y, const String& str, uint16_t color);
    void drawStringFast(int x, int y, const String& str, uint16_t color, uint16_t bgColor);

    // 计算字符串宽度（像素）
    int textWidth(const String& str);

    // 获取字符像素数据（旧 1-bit 模式, AA 模式返回空）
    String getPixelData(const String& str);

private:
    TFT_eSPI* _tft;
    File _fontFile;
    bool _isInit;
    bool _isAA = false;         // 新版 AA 模式标志

    String _fontFilePath;
    String _allUnicodes;

    int _totalFontCount;
    int _fontSize;
    int _binType;
    int _fontPage;
    int _unicodeBeginIdx;

    int _screenWidth;
    int _screenHeight;

    const char* _s64 = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ@#*$";

    // ===== AA 模式数据 (PSRAM) =====
    uint8_t*  _aaBlob   = nullptr;   // 整个文件 (PSRAM)
    uint16_t* _aaUni    = nullptr;   // unicode 数组 (sorted)
    uint8_t*  _aaAdv    = nullptr;   // advance 数组
    const uint8_t* _aaPixels = nullptr; // 像素数据起点 (blob 内)
    mutable uint16_t _buf[256] = {0}; // 16x16 合成缓冲 (const 方法内复用)

    // ===== 内部方法 =====
    String utf8ToUnicode(const String& utf8Str);
    String getCodeFromFile(const String& unicodeStr);
    String hexToPixelData(const String& hexStr);
    int calcFontPage(int fontSize, int binType);
    bool isAscii(unsigned char c);
    void drawGlyph(int x, int y, const String& pixelData, uint16_t color, bool isAsciiChar);
    void drawGlyph(int x, int y, const String& pixelData, uint16_t color, uint16_t bgColor, bool isAsciiChar);
    String charsToString(uint8_t* buf, int len);

    // AA 模式
    bool loadAA(const String& fontPath);
    int  findAA(uint16_t code) const;      // 二分查找 unicode
    int  textWidthAA(const String& str) const;
    void drawStringFastAA(int x, int y, const String& str, uint16_t fg, uint16_t bg) const;
    void compositeGlyph(int x, int y, int idx, uint16_t fg, uint16_t bg) const;
};

#endif // _XFONT_H_
