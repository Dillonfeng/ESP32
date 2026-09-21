/*
 * 电台管理模块 v2.0 - 支持大批量电台 (1600+)
 *
 * 特性:
 * - LittleFS 文件系统存储 (JSON格式)
 * - 支持批量导入: JSON / M3U / CSV
 * - 多级分类: 按地区、按类型、按字母索引
 * - 全文搜索 (电台名称、地区、分类)
 * - 收藏夹管理
 * - 最近播放记录
 * - 分页加载 (UI性能优化)
 * - 拼音首字母索引
 *
 * 存储格式:
 * {
 *   "version": 2,
 *   "count": 1680,
 *   "stations": [
 *     { "n": "经济之声", "u": "http://...", "r": "北京", "g": "新闻", "b": 128, "f": 2 },
 *     ...
 *   ]
 * }
 *
 * 字段缩写 (节省空间):
 * n = name, u = url, r = region, g = genre, b = bitrate, f = format
 */

#ifndef _RADIO_STATIONS_H_
#define _RADIO_STATIONS_H_

#include <Arduino.h>
#include <vector>
#include "config.h"

// 电台流格式
enum StreamFormat {
    STREAM_FORMAT_MP3 = 0,     // MP3 直链
    STREAM_FORMAT_AAC,         // AAC 直链
    STREAM_FORMAT_M3U8,        // HLS m3u8 播放列表
    STREAM_FORMAT_PLS,         // PLS 播放列表
    STREAM_FORMAT_OGG,         // OGG/Vorbis
    STREAM_FORMAT_FLAC,        // FLAC
    STREAM_FORMAT_WAV,         // WAV
    STREAM_FORMAT_M4A,         // M4A/AAC
    STREAM_FORMAT_UNKNOWN
};

// 排序方式
enum SortMode {
    SORT_BY_NAME = 0,          // 按名称
    SORT_BY_GENRE,             // 按分类
    SORT_BY_REGION,            // 按地区
    SORT_BY_BITRATE_DESC,      // 按码率降序
    SORT_BY_FAVORITE,          // 收藏优先
    SORT_BY_CUSTOM             // 自定义顺序
};

// 显示模式
enum DisplayMode {
    DISPLAY_ALL = 0,           // 全部电台
    DISPLAY_BY_GENRE,          // 按分类
    DISPLAY_BY_REGION,         // 按地区
    DISPLAY_FAVORITES,         // 收藏夹
    DISPLAY_RECENT,            // 最近播放
    DISPLAY_SEARCH             // 搜索结果
};

// 电台信息 (紧凑结构，节省内存)
typedef struct {
    char name[STATION_NAME_MAX_LEN];   // 电台名称
    char url[STATION_URL_MAX_LEN];     // 流地址
    char region[STATION_REGION_MAX_LEN]; // 地区
    char genre[STATION_GENRE_MAX_LEN];   // 分类/流派
    uint16_t bitrate;                   // 码率 (kbps)
    uint8_t  format;                    // 流格式 (StreamFormat)
    uint8_t  favorite;                  // 是否收藏 (0/1)
    uint32_t id;                        // 唯一ID
} StationInfo;

// 分类统计
typedef struct {
    char name[STATION_GENRE_MAX_LEN];
    int count;
} GenreStat;

// 地区统计
typedef struct {
    char name[STATION_REGION_MAX_LEN];
    int count;
} RegionStat;

// 分页信息
typedef struct {
    int pageIndex;       // 当前页
    int pageSize;        // 每页数量
    int totalPages;      // 总页数
    int totalItems;      // 总条目数
} PageInfo;

class RadioStations {
public:
    RadioStations();
    ~RadioStations();

    // 初始化（从文件系统加载）
    bool begin();

    // ============================================================
    // 基本查询
    // ============================================================

    // 获取电台总数
    int getCount() const { return _stations.size(); }

    // 获取指定索引的电台 (原始列表)
    StationInfo* getStation(int index);

    // 根据ID查找电台
    StationInfo* getStationById(uint32_t id);
    int findStationIndexById(uint32_t id);

    // 根据名称查找 (精确匹配)
    int findStationByName(const char* name);

    // ============================================================
    // 当前播放
    // ============================================================

    StationInfo* getCurrentStation();
    int getCurrentIndex() const { return _currentIndex; }
    uint32_t getCurrentId() const { return _currentId; }

    bool setCurrentStation(int index);
    bool setCurrentStationById(uint32_t id);
    bool setCurrentStationByName(const char* name);

    int nextStation();
    int prevStation();

    // ============================================================
    // 搜索功能
    // ============================================================

    // 全文搜索 (名称/地区/分类)
    int search(const char* keyword);
    int getSearchResultCount() const { return _searchResults.size(); }
    StationInfo* getSearchResult(int index);

    // 清除搜索结果
    void clearSearch();

    // ============================================================
    // 分类管理
    // ============================================================

    // 获取所有分类
    int getGenres(std::vector<GenreStat>& result);

    // 按分类获取电台
    int getStationsByGenre(const char* genre);

    // 获取所有地区
    int getRegions(std::vector<RegionStat>& result);

    // 按地区获取电台
    int getStationsByRegion(const char* region);

    // ============================================================
    // 收藏夹
    // ============================================================

    int getFavoriteCount() const { return _favoriteCount; }
    bool isFavorite(uint32_t id);
    bool toggleFavorite(uint32_t id);
    bool addFavorite(uint32_t id);
    bool removeFavorite(uint32_t id);
    void getFavorites(std::vector<StationInfo*>& result);

    // ============================================================
    // 最近播放
    // ============================================================

    int getRecentCount() const { return _recentIds.size(); }
    StationInfo* getRecent(int index);
    void addToRecent(uint32_t id);
    void clearRecent();

    // ============================================================
    // 显示模式与分页
    // ============================================================

    void setDisplayMode(DisplayMode mode, const char* filter = nullptr);
    DisplayMode getDisplayMode() const { return _displayMode; }

    int getDisplayCount() const;
    StationInfo* getDisplayItem(int displayIndex);

    // 分页
    void setPageSize(int size) { _pageInfo.pageSize = size; updatePageCount(); }
    int getPageSize() const { return _pageInfo.pageSize; }
    int getCurrentPage() const { return _pageInfo.pageIndex; }
    int getTotalPages() const { return _pageInfo.totalPages; }
    bool setPage(int page);
    bool nextPage();
    bool prevPage();

    // 排序
    void setSortMode(SortMode mode);
    SortMode getSortMode() const { return _sortMode; }

    // ============================================================
    // 批量导入/导出
    // ============================================================

    // 从JSON文件导入 (LittleFS)
    int importFromJSON(const char* filePath);

    // 从M3U文件导入
    int importFromM3U(const char* filePath);

    // 从CSV文件导入
    int importFromCSV(const char* filePath);

    // 导出到JSON文件
    bool exportToJSON(const char* filePath);

    // 从内存JSON字符串导入 (用于Web上传)
    int importFromJSONString(const char* jsonStr, int len);

    // 添加单个电台
    bool addStation(const StationInfo& station);

    // 删除电台
    bool removeStation(int index);
    bool removeStationById(uint32_t id);

    // 批量删除
    int removeAll();

    // ============================================================
    // 持久化
    // ============================================================

    bool saveToFile();
    bool loadFromFile();
    bool saveFavorites();
    bool loadFavorites();
    bool saveRecent();
    bool loadRecent();

    // ============================================================
    // 工具函数
    // ============================================================

    static const char* formatToString(StreamFormat format);
    static StreamFormat parseFormat(const char* url);
    static const char* formatDisplayName(StreamFormat format);

    // 获取电台名称首字母 (A-Z, #)
    static char getFirstLetter(const char* name);

private:
    std::vector<StationInfo> _stations;       // 所有电台
    std::vector<int> _displayIndices;         // 当前显示的索引列表
    std::vector<int> _searchResults;          // 搜索结果索引
    std::vector<uint32_t> _favoriteIds;       // 收藏ID列表
    std::vector<uint32_t> _recentIds;         // 最近播放ID列表

    int _currentIndex;
    uint32_t _currentId;
    uint32_t _nextId;

    DisplayMode _displayMode;
    SortMode _sortMode;
    PageInfo _pageInfo;
    char _filterGenre[STATION_GENRE_MAX_LEN];
    char _filterRegion[STATION_REGION_MAX_LEN];

    int _favoriteCount;
    bool _dirty;  // 数据是否需要保存

    // 构建显示列表
    void buildDisplayList();

    // 排序显示列表
    void sortDisplayList();

    // 更新分页信息
    void updatePageCount();

    // 加载默认内置电台
    void loadBuiltinStations();

    // 生成唯一ID
    uint32_t generateId();

    // 计算索引（二分查找辅助）
    static int compareByName(const void* a, const void* b);
    static int compareByGenre(const void* a, const void* b);
    static int compareByRegion(const void* a, const void* b);
};

// 全局电台管理实例
extern RadioStations g_stations;

#endif // _RADIO_STATIONS_H_
