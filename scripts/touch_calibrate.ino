/**
 * touch_calibrate.ino — XPT2046 触摸校准工具
 *
 * 用法:
 *   1. 单独编译烧录本文件 (替换 main.cpp, 只保留本文件)
 *   2. 串口 115200 看输出
 *   3. 依次点屏幕: 左上 → 右上 → 右下 → 左下 (每个角按住 1 秒)
 *   4. 串口输出 5 个值, 替换 main.cpp 里的 touchCalib[]
 *
 * 编译: 把本文件内容替换 src/main.cpp 即可 (或单独开 PlatformIO 项目)
 */
#include <TFT_eSPI.h>
#include <SPI.h>

TFT_eSPI tft = TFT_eSPI();

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);  // 标准 480x320 横向
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("XPT2046 Touch Calibrate");
  tft.println("Tap each corner:");
  tft.println("  TL -> TR -> BR -> BL");
  tft.println("(hold 1s each)");

  Serial.println("=== XPT2046 Calibration ===");
  Serial.println("Tap top-left corner and hold...");
  delay(3000);

  // 采集 4 个角
  struct Corner { uint16_t rawX, rawY; };
  Corner corners[4];
  const char* names[] = {"TL", "TR", "BR", "BL"};

  for (int i = 0; i < 4; i++) {
    // 等触摸
    bool got = false;
    uint16_t sumX = 0, sumY = 0;
    int samples = 0;

    while (!got && samples < 100) {
      int16_t x, y;
      int touched = tft.getTouch(&x, &y);
      if (touched > 0) {
        sumX += x; sumY += y;
        samples++;
        if (samples >= 50) got = true;  // 至少 50 个采样
      }
      delay(5);
    }

    if (samples > 0) {
      corners[i].rawX = sumX / samples;
      corners[i].rawY = sumY / samples;
      Serial.printf("%s: raw=(%u, %u)\n", names[i], corners[i].rawX, corners[i].rawY);
    } else {
      Serial.printf("%s: NO TOUCH DETECTED!\n", names[i]);
      corners[i].rawX = 300; corners[i].rawY = 300;
    }

    if (i < 3) {
      Serial.printf("Tap %s corner and hold...\n", names[i + 1]);
      tft.fillRect(0, 160, 480, 60, TFT_BLACK);
      tft.setCursor(10, 170);
      tft.printf("Next: %s", names[i + 1]);
    }
  }

  // 计算校准参数
  // 格式: {x0, x1, y0, y1, flags}
  // x0=最小X, x1=最大X, y0=最小Y, y1=最大Y
  // flags: bit0=rotate, bit1=inv_x, bit2=inv_y

  uint16_t x0 = corners[0].rawX, x1 = corners[1].rawX;  // TL.x, TR.x
  uint16_t y0 = corners[0].rawY, y1 = corners[2].rawY;  // TL.y, BR.y

  // 确保 x0 < x1, y0 < y1 (否则需要 invert)
  uint8_t flags = 0;
  bool rot = (corners[1].rawX - corners[0].rawX) < (corners[2].rawY - corners[0].rawY);
  // 如果 TR.x - TL.x < BR.y - TL.y, 说明面板物理方向需要 rotate
  if (rot) {
    // swap: 物理 X 轴对应显示 Y 轴
    uint16_t tx = x1;
    x1 = y1;
    y1 = tx;
    flags |= 0x01;  // rotate
  }

  // 检查是否需要 invert
  if (corners[2].rawX < corners[0].rawX) flags |= 0x02;  // inv_x
  if (corners[2].rawY < corners[0].rawY) flags |= 0x04;  // inv_y

  // 重新确定 min/max
  uint16_t rawXMin = min(min(corners[0].rawX, corners[1].rawX), min(corners[2].rawX, corners[3].rawX));
  uint16_t rawXMax = max(max(corners[0].rawX, corners[1].rawX), max(corners[2].rawX, corners[3].rawX));
  uint16_t rawYMin = min(min(corners[0].rawY, corners[1].rawY), min(corners[2].rawY, corners[3].rawY));
  uint16_t rawYMax = max(max(corners[0].rawY, corners[1].rawY), max(corners[2].rawY, corners[3].rawY));

  Serial.println();
  Serial.println("=== RESULT: replace touchCalib[] in main.cpp with ===");
  Serial.printf("uint16_t touchCalib[] = {%u, %u, %u, %u, 0x%02X};\n",
                rawXMin, rawXMax, rawYMin, rawYMax, flags);
  Serial.println();
  Serial.println("Then re-burn the normal firmware.");

  // 屏幕也显示
  tft.fillRect(0, 200, 480, 120, TFT_BLACK);
  tft.setCursor(10, 210);
  tft.printf("Calib: {%u, %u,", rawXMin, rawXMax);
  tft.setCursor(10, 235);
  tft.printf("%u, %u, 0x%02X};", rawYMin, rawYMax, flags);
  tft.setCursor(10, 270);
  tft.println("Replace touchCalib[] in main.cpp");
}

void loop() {
  // 持续显示原始触摸坐标供调试
  static uint32_t last = 0;
  if (millis() - last > 100) {
    int16_t x, y;
    int touched = tft.getTouch(&x, &y);
    if (touched > 0) {
      Serial.printf("raw: (%d, %d) z=%d\n", x, y, (int)touched);
      last = millis();
    }
  }
}
