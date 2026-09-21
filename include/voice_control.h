/*
 * 语音控制模块 - 小智AI
 * 硬件: INMP441 I2S麦克风
 * 功能: 离线唤醒 + 云端语音识别 + AI对话 + 收音机控制
 */

#ifndef _VOICE_CONTROL_H_
#define _VOICE_CONTROL_H_

#include <Arduino.h>
#include "config.h"

// 语音状态
enum VoiceState {
    VOICE_STATE_IDLE = 0,        // 空闲，等待唤醒
    VOICE_STATE_LISTENING,       // 已唤醒，正在听指令
    VOICE_STATE_PROCESSING,      // 正在处理/识别
    VOICE_STATE_SPEAKING,        // 正在播放回复
    VOICE_STATE_ERROR            // 错误
};

// 语音指令类型
enum VoiceCommand {
    CMD_NONE = 0,
    CMD_PLAY,                    // 播放
    CMD_PAUSE,                   // 暂停
    CMD_STOP,                    // 停止
    CMD_NEXT_STATION,            // 下一台
    CMD_PREV_STATION,            // 上一台
    CMD_VOLUME_UP,               // 音量+
    CMD_VOLUME_DOWN,             // 音量-
    CMD_SET_VOLUME,              // 设置音量到X%
    CMD_MUTE,                    // 静音
    CMD_UNMUTE,                  // 取消静音
    CMD_PLAY_STATION,            // 播放指定电台
    CMD_WEATHER,                 // 天气预报
    CMD_TIME,                    // 时间
    CMD_OPEN_PAGE,               // 打开页面
    CMD_MOOD,                    // 心情模式
    CMD_CHAT                     // 闲聊/其他
};

// 解析后的指令
typedef struct {
    VoiceCommand command;        // 指令类型
    char param1[64];             // 参数1
    char param2[32];             // 参数2
    int intValue;                // 数值参数
    float floatValue;            // 浮点参数
    char rawText[128];           // 原始识别文本
} ParsedCommand;

// 指令回调类型
typedef void (*VoiceCommandCallback)(const ParsedCommand& cmd);
typedef void (*VoiceStateCallback)(VoiceState state);

class VoiceControl {
public:
    VoiceControl();
    ~VoiceControl();

    // 初始化
    bool begin();

    // 主循环
    void loop();

    // 手动触发监听（比如按键触发）
    bool startListening(int timeoutMs = 5000);

    // 停止监听
    void stopListening();

    // 获取当前状态
    VoiceState getState() const { return _state; }

    // 获取最后识别的文本
    const char* getLastRecognizedText() const { return _lastRecognizedText; }

    // 获取最后回复的文本
    const char* getLastResponseText() const { return _lastResponseText; }

    // 设置指令回调
    void setCommandCallback(VoiceCommandCallback cb) { _commandCallback = cb; }

    // 设置状态回调
    void setStateCallback(VoiceStateCallback cb) { _stateCallback = cb; }

    // 解析文本指令（语音识别文本或手动输入）
    ParsedCommand parseCommand(const char* text);

    // 执行文本指令
    bool executeCommand(const char* text);

    // 唤醒词检测（简化版本，实际用离线唤醒库）
    bool detectWakeWord(const int16_t* audioData, int len);

    // 启用/禁用
    void setEnabled(bool enabled) { _enabled = enabled; }
    bool isEnabled() const { return _enabled; }

private:
    VoiceState _state;
    bool _enabled;
    bool _listening;
    uint32_t _listenTimeout;
    uint32_t _listenStartTime;

    char _lastRecognizedText[128];
    char _lastResponseText[256];

    VoiceCommandCallback _commandCallback;
    VoiceStateCallback _stateCallback;

    // I2S麦克风缓冲区
    int16_t* _micBuffer;
    int _micBufferSize;
    int _micReadIndex;

    // 设置状态
    void setState(VoiceState state);

    // 语音识别（云端）
    bool recognizeSpeech(const int16_t* audio, int len, char* result, int maxLen);

    // AI对话（云端）
    bool chatWithAI(const char* text, char* response, int maxLen);

    // 文本转语音（可选，用音频播放）
    bool textToSpeech(const char* text);

    // 关键词匹配
    bool matchKeyword(const char* text, const char* keyword);

    // 从文本中提取数字
    int extractNumber(const char* text);
};

// 全局语音控制实例
extern VoiceControl g_voiceControl;

#endif // _VOICE_CONTROL_H_
