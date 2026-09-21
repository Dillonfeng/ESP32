/*
 * 音频播放器模块
 * 基于 ESP32-audioI2S 库，支持 MP3/AAC/FLAC/M4A/HLS 等多种格式
 * 硬件: PCM5102A DAC + PAM8403 功放
 */

#ifndef _AUDIO_PLAYER_H_
#define _AUDIO_PLAYER_H_

#include <Arduino.h>
#include "Audio.h"
#include "config.h"

// 播放状态
enum PlayState {
    PLAY_STATE_STOPPED = 0,     // 已停止
    PLAY_STATE_CONNECTING,      // 连接中
    PLAY_STATE_BUFFERING,       // 缓冲中
    PLAY_STATE_PLAYING,         // 播放中
    PLAY_STATE_PAUSED,          // 已暂停
    PLAY_STATE_ERROR            // 错误
};

// VU表数据
typedef struct {
    float left;                 // 左声道音量 (0.0 - 1.0)
    float right;                // 右声道音量 (0.0 - 1.0)
    float leftPeak;             // 左声道峰值
    float rightPeak;            // 右声道峰值
    uint32_t peakHoldTimeLeft;  // 左峰值保持时间
    uint32_t peakHoldTimeRight; // 右峰值保持时间
} VUData;

// 音频信息
typedef struct {
    int sampleRate;             // 采样率
    int bitRate;                // 码率
    int channels;               // 声道数 (1=单声道, 2=立体声)
    uint32_t totalBytes;        // 已接收总字节数
    uint32_t decodeTime;        // 解码耗时
    bool isStereo;              // 是否立体声
} AudioInfo;

// 播放状态回调函数类型
typedef void (*PlayStateCallback)(PlayState state);
typedef void (*VUDataCallback)(const VUData& vu);

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    // 初始化音频播放器
    bool begin();

    // 播放指定URL
    bool play(const char* url);

    // 停止播放
    void stop();

    // 暂停/恢复
    void pause();
    void resume();
    bool togglePause();

    // 音量控制
    void setVolume(int volume);   // 0-100
    int getVolume() const { return _volume; }
    void volumeUp(int step = 5);
    void volumeDown(int step = 5);

    // 静音控制
    void setMute(bool mute);
    bool toggleMute();
    bool isMuted() const { return _muted; }

    // 获取播放状态
    PlayState getState() const { return _state; }
    bool isPlaying() const { return _state == PLAY_STATE_PLAYING; }
    bool isConnected() const { return _state == PLAY_STATE_PLAYING || _state == PLAY_STATE_BUFFERING; }

    // 获取音频信息
    const AudioInfo& getAudioInfo() const { return _audioInfo; }

    // 获取VU表数据
    const VUData& getVUData() const { return _vuData; }

    // 获取当前播放URL
    const char* getCurrentUrl() const { return _currentUrl; }

    // 设置状态回调
    void setStateCallback(PlayStateCallback cb) { _stateCallback = cb; }

    // 设置VU数据回调
    void setVUCallback(VUDataCallback cb) { _vuCallback = cb; }

    // 循环任务（需要在loop中调用）
    void loop();

    // 获取错误信息
    const char* getLastError() const { return _lastError; }

    // 流结束处理（由全局回调调用）
    void _handleEof();

    // 音频库实例（内部使用，公开以便回调访问）
    Audio _audio;

private:
    PlayState _state;
    int _volume;
    bool _muted;
    int _prevVolume;
    char _currentUrl[STATION_URL_MAX_LEN];
    char _lastError[128];

    AudioInfo _audioInfo;
    VUData _vuData;

    PlayStateCallback _stateCallback;
    VUDataCallback _vuCallback;

    uint32_t _lastVUUpdate;
    uint32_t _connectTimeout;
    uint32_t _connectStartTime;

    // 设置播放状态
    void setState(PlayState state);

    // 更新VU表数据
    void updateVUData();

    // 计算RMS音量
    float calculateRMS(const int16_t* samples, int count, int channelOffset);

    // 友元声明
    friend class Audio;
};

// 全局音频播放器实例
extern AudioPlayer g_audioPlayer;

#endif // _AUDIO_PLAYER_H_
