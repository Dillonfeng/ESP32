/*
 * recorder.h - 卡带录音机 (INMP441 -> TF 卡 WAV)
 * 2026-11-07: 验证 麦克风 I2S1(42/41/17) 与 TF 卡(HSPI) 并行无冲突, 为语音功能铺路。
 * 2026-11-08: 交互改为「顶栏 REC 只打开界面, 面板琴键 REC 才开始录」,
 *             支持多卷连录, PLAY 先弹磁带索引选带, ✕/STOP/BOOT 短按退出。
 */
#ifndef _RECORDER_H_
#define _RECORDER_H_

void recorderInit();        // setup() 末尾: BOOT 按键输入初始化 (不分配 PSRAM)
void recorderTick();        // loop() 每帧 (g_spiMutex 持锁区间内调用): 按键状态机 + 触摸 + 录音泵 + 卡带 UI
bool recorderActive();      // true=录音机界面占用中(待机面板/录音/回放), 主循环应跳过正常页面绘制与触摸消费
void recorderStartFromUi(); // 触摸入口: 收音机顶栏 REC 键 = 只打开录音机界面, 不开始录制 (幂等)
bool recorderBlockTouch();  // true=退出/停止那一次触摸会话未结束, 主循环应跳过 handleTouch

#endif // _RECORDER_H_
