/*
 * 天气预报模块
 * 支持和风天气API (QWeather)
 */

#ifndef _WEATHER_H_
#define _WEATHER_H_

#include <Arduino.h>
#include "config.h"

// 天气数据结构
typedef struct {
    char cityName[32];         // 城市名称
    char cityId[16];           // 城市ID
    float temperature;         // 当前温度 (°C)
    float feelsLike;           // 体感温度
    int humidity;              // 湿度 (%)
    float windSpeed;           // 风速 (m/s)
    int windDir;               // 风向角度
    char windDirText[16];      // 风向文字
    int pressure;              // 气压 (hPa)
    int visibility;            // 能见度 (km)
    char text[32];             // 天气状况文字描述
    int weatherCode;           // 天气状况代码
    char iconUrl[64];          // 天气图标URL
    float tempMin;             // 最低温
    float tempMax;             // 最高温
    char updateTime[32];       // 数据更新时间
    bool valid;                // 数据是否有效
} WeatherData;

// 天气预报（未来几天）
typedef struct {
    char fxDate[16];           // 预报日期
    float tempMax;             // 最高温
    float tempMin;             // 最低温
    char textDay[32];          // 白天天气
    char textNight[32];        // 夜间天气
    int weatherCodeDay;        // 白天天气代码
} ForecastDay;

class WeatherManager {
public:
    WeatherManager();
    ~WeatherManager();

    // 初始化
    bool begin();

    // 更新天气数据
    bool update();

    // 获取当前天气
    const WeatherData& getCurrentWeather() const { return _currentWeather; }

    // 获取预报数据（最多7天）
    const ForecastDay* getForecast(int day) const;
    int getForecastCount() const { return _forecastCount; }

    // 设置城市
    bool setCity(const char* cityId, const char* cityName = nullptr);
    const char* getCityName() const { return _currentWeather.cityName; }
    const char* getCityId() const { return _currentWeather.cityId; }

    // 获取上次更新时间
    uint32_t getLastUpdateTime() const { return _lastUpdateTime; }

    // 是否需要更新
    bool needsUpdate() const {
        return (millis() - _lastUpdateTime > WEATHER_UPDATE_INTERVAL) || !_currentWeather.valid;
    }

    // 天气代码转表情/图标
    static const char* weatherCodeToEmoji(int code);
    static const char* weatherCodeToIconName(int code);

private:
    WeatherData _currentWeather;
    ForecastDay _forecast[7];
    int _forecastCount;
    uint32_t _lastUpdateTime;
    bool _initialized;

    // HTTP请求获取实时天气
    bool fetchCurrentWeather();

    // HTTP请求获取天气预报
    bool fetchForecast();

    // 解析JSON天气数据
    bool parseCurrentWeather(const String& json);
    bool parseForecast(const String& json);

    // 风向角度转文字
    static void windDegreeToText(int degree, char* buf, int bufSize);
};

// 全局天气管理实例
extern WeatherManager g_weather;

#endif // _WEATHER_H_
