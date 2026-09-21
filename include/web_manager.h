/*
 * Web管理界面模块
 * 功能:
 * - 电台批量上传 (JSON / M3U / CSV)
 * - 电台列表管理 (增删改查)
 * - 系统设置 (WiFi/天气/音量等)
 * - 播放控制
 *
 * 访问方式: http://<设备IP>
 */

#ifndef _WEB_MANAGER_H_
#define _WEB_MANAGER_H_

#include <Arduino.h>
#include "config.h"

// Web服务器端口
#define WEB_SERVER_PORT     80

class WebManager {
public:
    WebManager();
    ~WebManager();

    // 初始化Web服务器
    bool begin();

    // 处理客户端请求 (需要在loop中调用)
    void handleClient();

    // 是否已启动
    bool isStarted() const { return _started; }

    // 获取访问URL
    String getURL() const;

private:
    bool _started;

    // 处理各页面
    void handleRoot();
    void handleStations();
    void handleUpload();
    void handleSettings();
    void handleAPI();
    void handleNotFound();

    // API接口
    void apiGetStationList();
    void apiGetStationCount();
    void apiSearchStations();
    void apiAddStation();
    void apiDeleteStation();
    void apiPlayStation();
    void apiImportStatus();
    void apiGetGenres();
    void apiGetRegions();

    // 上传处理
    bool handleFileUpload();

    // 生成HTML页面
    String renderHeader(const char* title);
    String renderFooter();
    String renderStationListPage();
    String renderUploadPage();
    String renderSettingsPage();
    String renderPlayerPage();

    // 辅助函数
    String getContentType(const String& filename);
};

// 全局Web管理实例
extern WebManager g_webManager;

#endif // _WEB_MANAGER_H_
