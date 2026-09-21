#include "tft_touch.h"

TFTTouch::TFTTouch(TFT_eSPI* tft) : _tft(tft), _touchStarted(false),
    _lastTouchState(false), _lastX(0), _lastY(0), _pressTime(0),
    _buttons(nullptr), _buttonCount(0), _buttonsInit(false) {
}

void TFTTouch::begin() {
    _touchStarted = true;
}

// 检测是否有触摸
bool TFTTouch::isTouched() {
    if (!_touchStarted) return false;
    uint16_t tx, ty;
    return _tft->getTouch(&tx, &ty) > 0;
}

// 获取触摸坐标（已校准到屏幕坐标）
bool TFTTouch::getPoint(int& x, int& y) {
    if (!_touchStarted) return false;

    uint16_t tx, ty;
    if (_tft->getTouch(&tx, &ty) > 0) {
        // TFT_eSPI getTouch 已经做了校准，直接返回
        x = tx;
        y = ty;
        return true;
    }
    return false;
}

// 检测点击事件（按下→松开算一次点击）
bool TFTTouch::isClicked() {
    int x, y;
    bool current = getPoint(x, y);

    if (current && !_lastTouchState) {
        // 按下
        _lastX = x;
        _lastY = y;
        _pressTime = millis();
        _lastTouchState = true;
        return false;
    }

    if (!current && _lastTouchState) {
        // 松开 → 判定为点击
        _lastTouchState = false;
        unsigned long duration = millis() - _pressTime;
        if (duration > 20 && duration < 1000) {
            return true;
        }
    }

    _lastTouchState = current;
    return false;
}

// ============================================================
// 按钮管理
// ============================================================

void TFTTouch::initButtons(int count) {
    if (_buttons != nullptr) {
        delete[] _buttons;
    }
    _buttons = new TouchButton[count];
    _buttonCount = count;
    _buttonsInit = true;

    for (int i = 0; i < count; i++) {
        _buttons[i] = {0, 0, 0, 0, "", 0xFFFF, 0x1082, BTN_STYLE_ROUND, false, false, i};
    }
}

void TFTTouch::setButton(int index, int x, int y, int w, int h,
                         const char* label, uint16_t color,
                         BtnStyle style, int id) {
    if (index < 0 || index >= _buttonCount) return;
    _buttons[index].x = x;
    _buttons[index].y = y;
    _buttons[index].w = w;
    _buttons[index].h = h;
    _buttons[index].label = label;
    _buttons[index].color = color;
    _buttons[index].style = style;
    if (id >= 0) _buttons[index].id = id;
    _buttons[index].pressed = false;
    _buttons[index].lastPressed = false;
}

void TFTTouch::setButtonBgColor(int index, uint16_t bgColor) {
    if (index < 0 || index >= _buttonCount) return;
    _buttons[index].bgColor = bgColor;
}

void TFTTouch::drawButton(int index) {
    if (index < 0 || index >= _buttonCount) return;
    TouchButton& btn = _buttons[index];

    uint16_t fill = btn.pressed ? btn.color : btn.bgColor;
    uint16_t text = btn.pressed ? btn.bgColor : btn.color;

    switch (btn.style) {
        case BTN_STYLE_ROUND:
            _tft->fillRoundRect(btn.x, btn.y, btn.w, btn.h, 6, fill);
            _tft->drawRoundRect(btn.x, btn.y, btn.w, btn.h, 6, btn.color);
            break;
        case BTN_STYLE_RECT:
            _tft->fillRect(btn.x, btn.y, btn.w, btn.h, fill);
            _tft->drawRect(btn.x, btn.y, btn.w, btn.h, btn.color);
            break;
        case BTN_STYLE_ICON:
            _tft->fillRoundRect(btn.x, btn.y, btn.w, btn.h, 8, fill);
            _tft->drawRoundRect(btn.x, btn.y, btn.w, btn.h, 8, btn.color);
            break;
    }

    // 绘制文字
    _tft->setTextColor(text);
    _tft->setTextSize(1);
    int textW = _tft->textWidth(btn.label);
    _tft->setCursor(btn.x + (btn.w - textW) / 2, btn.y + (btn.h - 8) / 2);
    _tft->print(btn.label);
}

void TFTTouch::drawButtons() {
    for (int i = 0; i < _buttonCount; i++) {
        drawButton(i);
    }
}

bool TFTTouch::isInButton(int index, int x, int y) {
    if (index < 0 || index >= _buttonCount) return false;
    TouchButton& btn = _buttons[index];
    return (x >= btn.x && x < btn.x + btn.w &&
            y >= btn.y && y < btn.y + btn.h);
}

// 处理触摸事件，返回被点击的按钮ID（-1=无点击）
int TFTTouch::handleTouch() {
    if (!_buttonsInit || _buttons == nullptr || _buttonCount == 0) return -1;

    int x, y;
    bool currentTouched = getPoint(x, y);

    // 更新按钮按下状态
    if (currentTouched) {
        for (int i = 0; i < _buttonCount; i++) {
            bool inBtn = isInButton(i, x, y);
            if (inBtn != _buttons[i].pressed) {
                _buttons[i].pressed = inBtn;
                drawButton(i);
            }
        }
    } else {
        // 没有触摸时，如果之前有按钮被按下，触发点击
        for (int i = 0; i < _buttonCount; i++) {
            if (_buttons[i].pressed) {
                _buttons[i].pressed = false;
                drawButton(i);
                // 这是一个点击事件
                return _buttons[i].id;
            }
        }
    }

    return -1; // 无点击
}
