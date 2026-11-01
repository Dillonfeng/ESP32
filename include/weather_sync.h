// ============================================================
// weather_sync.h — 实时天气 (和风 QWeather) 在线拉取 + LittleFS 缓存
// 失败 -> 保留上次缓存 (绝不把页面弄空/弄乱)。
//
// 数据源对齐 D:\WinCalender 老黄历.hta: 专属 Host HTTPS (WiFiClientSecure
// setInsecure), 免费档接口: weather/now + weather/7d + airquality current|daily
// + weatheralert (气象预警)。
// 台风实况改走中国气象局台风网 (typhoon.nmc.cn, 免费/无 key/明文 HTTP)——
// 和风 tropical/storm-list|storm-track 属"台风和海洋"收费产品, 已不再调用。
// key/host 见 config.h WEATHER_API_KEY / WEATHER_API_HOST。
// 位置由 geo::pinned() 定位省份 -> 映射到省会城市经纬度 (无 GPS, 离线兜底广州)。
//
// 数据全部存固定 char/数值缓冲 (无 String 拼接 -> 无 realloc -> 无跨核
// use-after-free, 同 holiday_sync namePool 教训)。
// ============================================================
#pragma once
#include <Arduino.h>

namespace weather {

// 单份天气数据 (全部按值/固定缓冲, 指针永不悬垂)
struct WData {
  bool   ok = false;         // 是否有有效数据 (缓存或刚拉取)
  // 当前
  float  temp = 0, feels = 0, hi = 0, lo = 0;
  float  pressure = 0, uv = 0;
  bool   uvRealtime = false;   // uv 是否来自 Open-Meteo 实时值 (false=和风日值兜底)
  int    humidity = 0, windSpeed = 0, windScale = 0;
  int    code = 0;           // open-meteo WMO 天气代码 (当前)
  int    icon = 0;           // 映射后的图标 0..7 (wicons)
  char   windDir[16];        // 16 方位中文名不含"风", 如 "东北偏北" (UI 补"风")
  char   cond[24];           // 如 "多云"
  char   city[24];           // 显示城市名 (GeoAPI adm2, 如 "深圳")
  char   district[16];       // 区县名 (GeoAPI name, 如 "龙岗"; 空=无)
  char   date[16];           // "10-04 周日"
  char   update[8];          // "15:30"
  char   sunrise[8];         // "06:20"
  char   sunset[8];          // "18:08"
  float  visKm = 0;          // 能见度 km (current.visibility)
  int    elevM = 0;          // 海拔 m (open-meteo 模型地形 elevation, 随坐标固定; 0=未知)
  int    aqi = 0;            // 空气质量指数 (air-quality API us_aqi; 0=无数据)
  int    pm25 = 0, pm10 = 0; // μg/m³
  // 7 天预报
  char   dayLabel[7][8];     // 今天/明天/周一..周五
  char   dayDate[7][6];      // "10/22"
  char   dayCond[7][18];     // 晴/多云/晴间多云.. (中文 3 字节/字, 需 >=16 才放得下 5 字)
  int    dayIcon[7];         // 0..15 (16 类: 晴/多云/多云转晴/阴/小雨/中雨/大雨/雷阵雨/雨夹雪/冻雨/小雪/大雪/雾/霾/沙尘/大风)
  int    dayHi[7], dayLo[7];
  int    dayHum[7];          // 日均湿度 % (hourly 平均)
  int    dayAqi[7];          // 日 AQI: 今天=实时实测; 未来=和风daily, 缺的用 open-meteo PM 折算
  // 气象预警 (和风 weatheralert; alertCount=0 无预警)
  int    alertCount = 0;
  char   alertTitle[40];     // "暴雨橙色预警"
  char   alertColor[8];      // "蓝"/"黄"/"橙"/"红"/"白"
  char   alertDesc[200];     // 预警正文 (弹窗用, 截断)
  char   alertTime[16];      // "10-24 18:00"
  char   alertSender[32];    // 发布单位 "深圳市气象台"
  char   alertSeverity[8];   // 严重程度 "一般"/"较重"/"严重"/"特别严重"
  // 台风 (中国气象局台风网免费源; 预警 eventType=1001 也进 typhoon 字段)
  bool   typhoon = false;
  char   typhoonName[24];    // "台风烟花" (顶部按钮/弹窗标题)
  char   typhoonDesc[200];   // 兜底正文 (仅"预警有台风但无实况"时用)
  // 活跃台风列表 (NMC list 里 status=="start" 的, 最多 TY_MAX 个)
  // 弹窗一次全部列出, 内容超出窗口时出滚动条 (上下拖动查看)
  enum { TY_MAX = 3 };
  struct TyStorm {
    char  name[24];          // "烟花" (中文名)
    char  type[16];          // "强台风"
    float lat = 0, lon = 0;  // 中心
    int   wind = 0, press = 0;   // 最大风速 m/s, 中心气压 hPa
    char  moveDir[16];       // 移向中文 "西南偏西" (12B+NUL, 必须 >=13)
    int   moveSpeed = 0;     // 移速 km/h
    int   r64[4] = {0,0,0,0};    // 64节风圈 东北/东南/西南/西北 km (0=无)
  };
  int     tyCount = 0;       // 有实况的活跃台风数 (0..TY_MAX)
  TyStorm ty[TY_MAX];
  char   tyTime[8];          // "15:47" 抓取时刻 (所有台风共用)
};

void init();        // setup: 联网前先读缓存 (幂等)
void tick();        // loop: 快速(10min: 实况+预警+实时UV) / 全量(60min: 7天+空气+台风+UV日值) 分链路刷新
void forceRefresh();// UI 刷新按钮: 下一次 tick 立即拉取 (fast+full)
bool isDaytime();   // 当前是否在日出~日落之间 (紫外线等昼夜区分显示用)
// 城市弹窗手动选城: 覆盖自动定位坐标, 立即生效并触发下次 tick 刷新; city=nullptr 恢复自动定位。
void setManualCity(const char* city, const char* district, float lat, float lon);
float locLat();     // 最近一次拉取使用的纬度 (手动选城时=手动坐标; 台风相对位置按此算)
float locLon();
const WData& get(); // 当前数据 (无数据时 ok=false, 调用方退回 mock)
unsigned seq();     // 每次成功刷新 +1 (用于 UI 判断"数据是否变化" -> 重绘天气页)
} // namespace weather
