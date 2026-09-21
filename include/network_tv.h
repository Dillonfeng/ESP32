/*
 * 网络电视模块 - MJPEG 视频流播放器
 * 支持 MJPEG 格式的网络摄像头和IP电视流
 *
 * 注意: ESP32-S3 的性能有限，建议使用 320x240 或更低分辨率的MJPEG流
 * 帧率大约 10-15 FPS
 */

#ifndef _NETWORK_TV_H_
#define _NETWORK_TV_H_

#include <Arduino.h>
#include "config.h"

// 电视频道信息
typedef struct {
    char name[64];              // 频道名称
    char url[256];              // MJPEG流地址
    char region[32];            // 地区
    int width;                  // 视频宽度
    int height;                 // 视频高度
    bool isAudio;               // 是否有音频
} TVChannel;

// 播放状态
enum TVPlayState {
    TV_STATE_STOPPED = 0,
    TV_STATE_CONNECTING,
    TV_STATE_PLAYING,
    TV_STATE_ERROR
};

class NetworkTV {
public:
    NetworkTV();
    ~NetworkTV();

    // 初始化
    bool begin();

    // 播放指定频道
    bool play(int channelIndex);
    bool play(const char* url);

    // 停止播放
    void stop();

    // 切换频道
    void nextChannel();
    void prevChannel();

    // 获取状态
    TVPlayState getState() const { return _state; }
    bool isPlaying() const { return _state == TV_STATE_PLAYING; }

    // 获取当前频道
    const TVChannel* getCurrentChannel() const {
        if (_currentIndex >= 0 && _currentIndex < _channelCount) {
            return &_channels[_currentIndex];
        }
        return nullptr;
    }

    int getCurrentIndex() const { return _currentIndex; }
    int getChannelCount() const { return _channelCount; }

    // 获取频道列表
    const TVChannel* getChannel(int index) const {
        if (index >= 0 && index < _channelCount) {
            return &_channels[index];
        }
        return nullptr;
    }

    // 主循环 (需要在loop中调用)
    void loop();

    // 帧回调 (用于显示)
    typedef void (*FrameCallback)(const uint8_t* jpegData, int len, int width, int height);
    void setFrameCallback(FrameCallback cb) { _frameCallback = cb; }

    // 设置显示区域
    void setDisplayArea(int x, int y, int w, int h);

private:
    TVPlayState _state;
    int _currentIndex;
    int _channelCount;
    TVChannel _channels[20];    // 最多20个频道

    uint8_t* _frameBuffer;
    int _frameBufferSize;

    FrameCallback _frameCallback;

    int _displayX;
    int _displayY;
    int _displayW;
    int _displayH;

    uint32_t _lastFrameTime;
    int _frameCount;
    float _fps;

    // 加载默认频道
    void loadDefaultChannels();

    // 解析MJPEG流
    bool parseMJPEGStream(const char* url);

    // 查找JPEG帧边界
    int findJPEGStart(const uint8_t* buffer, int len);
    int findJPEGEnd(const uint8_t* buffer, int len);

    // 解码并显示JPEG (使用TJpgDec)
    bool decodeAndDisplayJPEG(const uint8_t* jpegData, int len);
};

// 全局网络电视实例
extern NetworkTV g_networkTV;

#endif // _NETWORK_TV_H_
