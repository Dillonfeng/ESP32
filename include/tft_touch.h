/*
 * 触摸屏管理模块
 * 基于 TFT_eSPI 的 XPT2046 触摸
 * 提供按钮控件和触摸事件检测
 */

#ifndef _TFT_TOUCH_H_
#define _TFT_TOUCH_H_

#include <Arduino.h>
#include <TFT_eSPI.h>

// 触摸按钮样式
enum BtnStyle {
    BTN_STYLE_ICON,     // 图标按钮（底部导航栏）
    BTN_STYLE_RECT,     // 矩形按钮（功能按钮）
    BTN_STYLE_ROUND,    // 圆角按钮
};

// 触摸按钮定义
struct TouchButton {
    int x, y;           // 位置
    int w, h;           // 尺寸
    const char* label;  // 文字标签
    uint16_t color;     // 按钮颜色
    uint16_t bgColor;   // 背景色
    BtnStyle style;     // 样式
    bool pressed;       // 当前是否被按下
    bool lastPressed;   // 上次状态
    int id;             // 按钮ID
};

class TFTTouch {
public:
    TFTTouch(TFT_eSPI* tft);

    // 初始化
    void begin();

    // 检测是否有触摸（非阻塞）
    bool isTouched();

    // 获取触摸坐标（已校准到屏幕坐标）
    bool getPoint(int& x, int& y);

    // 检测点击事件（按下→松开算一次点击）
    bool isClicked();

    // ============================================================
    // 按钮管理
    // ============================================================

    // 初始化按钮数组
    void initButtons(int count);

    // 设置按钮属性
    void setButton(int index, int x, int y, int w, int h,
                   const char* label, uint16_t color,
                   BtnStyle style = BTN_STYLE_ROUND,
                   int id = -1);

    // 设置按钮背景色
    void setButtonBgColor(int index, uint16_t bgColor);

    // 绘制所有按钮
    void drawButtons();

    // 绘制单个按钮
    void drawButton(int index);

    // 处理触摸事件，返回被点击的按钮ID（-1=无点击）
    int handleTouch();

    // 检查某个坐标是否在某个按钮内
    bool isInButton(int index, int x, int y);

private:
    TFT_eSPI* _tft;
    bool _touchStarted;

    // 触摸状态
    bool _lastTouchState;
    int _lastX, _lastY;
    unsigned long _pressTime;

    // 按钮数组
    TouchButton* _buttons;
    int _buttonCount;
    bool _buttonsInit;

    // 坐标校准
    int _calibrateX(int raw);
    int _calibrateY(int raw);
};

#endif // _TFT_TOUCH_H_
