/*
 * 心情电台模块
 * 根据心情推荐对应风格的电台
 */

#ifndef _MOOD_RADIO_H_
#define _MOOD_RADIO_H_

#include <Arduino.h>
#include "config.h"

// 心情类型
enum MoodType {
    MOOD_HAPPY = 0,      // 开心 - 欢快动感
    MOOD_RELAX,          // 放松 - 轻音乐/自然
    MOOD_SLEEP,          // 助眠 - 白噪音/冥想
    MOOD_ENERGETIC,      // 活力 - 运动/摇滚
    MOOD_NOSTALGIC,      // 怀旧 - 经典老歌
    MOOD_MEDITATE,       // 冥想 - 禅意/静心
    MOOD_COUNT
};

// 心情信息
typedef struct {
    MoodType mood;
    const char* name;        // 心情名称
    const char* emoji;       // 表情
    const char* description; // 描述
    uint16_t color;          // 主题色
    const char** stations;   // 推荐电台列表
    int stationCount;        // 电台数量
} MoodInfo;

class MoodRadio {
public:
    MoodRadio();
    ~MoodRadio();

    // 初始化
    bool begin();

    // 获取心情数量
    int getMoodCount() const { return MOOD_COUNT; }

    // 获取心情信息
    const MoodInfo* getMoodInfo(MoodType mood) const;
    const MoodInfo* getMoodInfo(int index) const;

    // 设置当前心情
    bool setMood(MoodType mood);
    bool setMood(const char* moodName);

    // 获取当前心情
    MoodType getCurrentMood() const { return _currentMood; }
    const MoodInfo* getCurrentMoodInfo() const;

    // 获取当前心情的推荐电台
    int getRecommendedStations(int* stationIndices, int maxCount);

    // 播放对应心情的随机电台
    bool playRandomStation();

    // 根据心情选择下一个电台
    bool nextStation();

    // 获取心情对应的背景渐变色
    void getMoodGradient(uint16_t& color1, uint16_t& color2) const;

    // 心情名称转枚举
    static MoodType nameToMood(const char* name);
    static const char* moodToName(MoodType mood);

private:
    MoodType _currentMood;
    int _currentStationIndex;  // 当前心情下的电台索引

    // 各心情的电台列表
    static const char* _happyStations[];
    static const char* _relaxStations[];
    static const char* _sleepStations[];
    static const char* _energeticStations[];
    static const char* _nostalgicStations[];
    static const char* _meditateStations[];

    static MoodInfo _moods[MOOD_COUNT];
};

// 全局心情电台实例
extern MoodRadio g_moodRadio;

#endif // _MOOD_RADIO_H_
