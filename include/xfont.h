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

    // 缩放绘制一个中文角标字符 (16x16 AA 灰度 -> dst x dst, 面积平均+逐通道合成)
    void drawScaledChar(int x, int y, uint16_t ch, int dst, uint16_t fg, uint16_t bg) const;
    // 放大绘制一个中文大标题字符 (源 AA 灰度 -> dst x dst, 顶角最近邻+逐通道合成,
    // dst 可达 40; 用于黄历大标题等比字库更大的中文场景)。
    // 源字形尺寸 = 字库原生分辨率 (_aaSize: ver1=16x16, ver2=32x32)。
    void drawBigChar(int x, int y, uint16_t ch, int dst, uint16_t fg, uint16_t bg) const;
    // 当前 AA 字库的原生字形边长 (16 或 32)。drawBigChar 依此取样。
    int aaSize() const { return _aaSize; }
    // 该 unicode 是否在 AA 字库中 (用于大字缺字回退)。非 AA 模式返回 false。
    bool hasGlyph(uint16_t ch) const { return _isAA && findAA(ch) >= 0; }

    // 获取字符像素数据（旧 1-bit 模式, AA 模式返回空）
    String getPixelData(const String& str);

private:
    TFT_eSPI* _tft;
    File _fontFile;
    bool _isInit;
    bool _isAA = false;         // 新版 AA 模式标志
    int  _aaSize = 16;          // AA 字库原生字形边长 (ver1=16 ver2=32 ver3=64 ver4=24超采样16px); 决定 compositeGlyph/drawScaledChar/drawBigChar 取样

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
