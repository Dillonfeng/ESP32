/*
 * 显示与UI管理模块
 * 硬件: ST7796S 4.0寸 SPI TFT 触摸屏 (480x320)
 * 库: TFT_eSPI + LVGL
 * 功能: VU表、电台信息、立体声指示、天气显示、电台列表等
 */

#ifndef _DISPLAY_UI_H_
#define _DISPLAY_UI_H_

#include <Arduino.h>
#include "config.h"
#include <TFT_eSPI.h>
#include <lvgl.h>
#include "audio_player.h"
#include "radio_stations.h"

// UI 页面枚举
enum UIPage {
    PAGE_NOW_PLAYING = 0,   // 播放主界面（VU表+电台信息）
    PAGE_STATION_LIST,      // 电台列表
    PAGE_WEATHER,           // 天气预报
    PAGE_SETTINGS,          // 设置
    PAGE_VOICE_CONTROL,     // 语音控制
    PAGE_NETWORK_TV,        // 网络电视
    PAGE_MOOD,              // 心情
    PAGE_COUNT
};

// 主题颜色
typedef struct {
    uint16_t bgPrimary;     // 主背景色
    uint16_t bgSecondary;   // 次背景色
    uint16_t textPrimary;   // 主文字色
    uint16_t textSecondary; // 次文字色
    uint16_t accent;        // 强调色
    uint16_t vuLeft;        // 左声道VU颜色
    uint16_t vuRight;       // 右声道VU颜色
    uint16_t vuPeak;        // VU峰值颜色
    uint16_t stereoOn;      // 立体声指示颜色
    uint16_t stereoOff;     // 单声道指示颜色
} UITheme;

// 显示管理器
class DisplayUI {
public:
    DisplayUI();
    ~DisplayUI();

    // 初始化显示
    bool begin();

    // 主循环（需要在loop中调用）
    void loop();

    // 页面切换
    void showPage(UIPage page);
    UIPage getCurrentPage() const { return _currentPage; }
    void nextPage();
    void prevPage();

    // 更新播放界面
    void updateNowPlaying(const StationInfo* station, const VUData& vu, const AudioInfo& audioInfo);

    // 更新电台列表
    void updateStationList(int selectedIndex);

    // 更新天气界面
    void updateWeather(const char* city, const char* temp, const char* desc, const char* icon);

    // 更新语音控制状态
    void updateVoiceStatus(const char* status, bool listening);

    // 显示音量变化
    void showVolumePopup(int volume, bool muted);

    // 显示提示消息
    void showToast(const char* message, int duration = 2000);

    // 设置背光亮度 (0-100)
    void setBrightness(int brightness);
    int getBrightness() const { return _brightness; }

    // 获取TFT对象（高级用）
    TFT_eSPI* getTFT() { return &_tft; }

    // LVGL 显示驱动回调（内部使用）
    static void lvglFlushCallback(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map);
    static void lvglTouchRead(lv_indev_t* indev, lv_indev_data_t* data);

private:
    TFT_eSPI _tft;
    UIPage _currentPage;
    int _brightness;
    bool _initialized;

    // LVGL 缓冲区
    static lv_color_t* _lvBuf1;
    static lv_color_t* _lvBuf2;
    static lv_display_t* _lvDisplay;
    static lv_indev_t* _lvIndev;

    // UI 对象
    lv_obj_t* _pageNowPlaying;
    lv_obj_t* _pageStationList;
    lv_obj_t* _pageWeather;
    lv_obj_t* _pageSettings;
    lv_obj_t* _pageVoice;
    lv_obj_t* _pageTV;
    lv_obj_t* _pageMood;

    // 播放界面元素
    lv_obj_t* _lblStationName;
    lv_obj_t* _lblStationRegion;
    lv_obj_t* _lblFormat;
    lv_obj_t* _lblBitrate;
    lv_obj_t* _lblStereo;
    lv_obj_t* _lblTime;
    lv_obj_t* _imgWeatherIcon;
    lv_obj_t* _lblTemp;
    lv_obj_t* _vuBarLeft;
    lv_obj_t* _vuBarRight;
    lv_obj_t* _vuPeakLeft;
    lv_obj_t* _vuPeakRight;
    lv_obj_t* _lblVolume;
    lv_obj_t* _barVolume;
    lv_obj_t* _btnPrev;
    lv_obj_t* _btnNext;
    lv_obj_t* _btnPlayPause;

    // 电台列表元素
    lv_obj_t* _listStations;
    lv_obj_t* _labelCategory;

    // 天气界面元素
    lv_obj_t* _lblWeatherCity;
    lv_obj_t* _lblWeatherTemp;
    lv_obj_t* _lblWeatherDesc;
    lv_obj_t* _lblWeatherDetail;

    // 主题
    UITheme _theme;

    // Toast
    lv_obj_t* _toastLabel;
    lv_timer_t* _toastTimer;

    // 初始化各页面
    void initNowPlayingPage();
    void initStationListPage();
    void initWeatherPage();
    void initSettingsPage();
    void initVoicePage();
    void initTVPage();
    void initMoodPage();

    // 绘制VU表（直接绘制，高性能）
    void drawVUMeter(float leftLevel, float rightLevel, float leftPeak, float rightPeak);

    // 触摸读取
    bool readTouch(int& x, int& y);

    // Toast 定时器回调
    static void toastTimerCallback(lv_timer_t* timer);
};

// 全局显示管理实例
extern DisplayUI g_display;

#endif // _DISPLAY_UI_H_
