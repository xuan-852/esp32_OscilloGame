#include <Arduino.h>
#include "pins.h"
#include "freertos.h"
#include "DACoutput.h"
#include "vector_draw.h"
#include "FS.h"
#include "SD_MMC.h"
#include "dac8554.h"

DAC8554 dac(10,11,12); // CS, MOSI, SCLK

// --- Encoder Logic ---
volatile int32_t encoderValue = 0;

void IRAM_ATTR readEncoderISR() {
  static uint8_t old_AB = 3; // Assume start at 11 (pullup)
  uint8_t enc_A = digitalRead(EN_A);
  uint8_t enc_B = digitalRead(EN_B);
  uint8_t new_AB = (enc_A << 1) | enc_B;
  
  if (old_AB != new_AB) {
      // Quadrature lookup table
      // Index: (old_AB << 2) | new_AB
      static const int8_t table[16] = {
          0, -1, 1, 0,  // 00 -> ...
          1, 0, 0, -1,  // 01 -> ...
          -1, 0, 0, 1,  // 10 -> ...
          0, 1, -1, 0   // 11 -> ...
      };
      encoderValue += table[(old_AB << 2) | new_AB];
      old_AB = new_AB;
  }
}
// ---------------------

void listDir(fs::FS &fs, const char * dirname, uint8_t levels) {
    Serial.printf("Listing directory: %s\n", dirname);

    File root = fs.open(dirname);
    if(!root){
        Serial.println("Failed to open directory");
        return;
    }
    if(!root.isDirectory()){
        Serial.println("Not a directory");
        return;
    }

    File file = root.openNextFile();
    while(file){
        if(file.isDirectory()){
            Serial.print("  DIR : ");
            Serial.println(file.name());
            if(levels){
                listDir(fs, file.path(), levels -1);
            }
        } else {
            Serial.print("  FILE: ");
            Serial.print(file.name());
            Serial.print("  SIZE: ");
            Serial.println(file.size());
        }
        file = root.openNextFile();
    }
}

void setup() {
  // 初始化串口
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nESP32-S3 Hardware Data Output");
  
  Serial.printf("Flash Size: %d MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.printf("PSRAM Size: %d MB\n", ESP.getPsramSize() / (1024 * 1024));

  // 初始化手柄按键 (使用内部上拉，按下为 LOW)
  pinMode(JOY1_SW, INPUT_PULLUP);
  pinMode(JOY2_SW, INPUT_PULLUP);
  pinMode(JOY_A, INPUT_PULLDOWN);
  pinMode(JOY_B, INPUT_PULLDOWN);

  // Encoder Pins
  pinMode(EN_A, INPUT_PULLUP);
  pinMode(EN_B, INPUT_PULLUP);
  pinMode(EN_S, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(EN_A), readEncoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(EN_B), readEncoderISR, CHANGE);

  // 配置 ADC 分辨率 (默认12位: 0-4095)
  analogReadResolution(12);

  // 初始化 SD 卡
  // setPins(clk, cmd, d0)
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  // begin(mountpoint, mode1bit, format_if_mount_failed, freq)
  // 降低频率到 20MHz 或更低以减少干扰
  if (!SD_MMC.begin("/sdcard", true, false, 20000)) {
    Serial.println("Card Mount Failed");
  } else {
    Serial.println("SD Card Mounted Successfully");
    uint8_t cardType = SD_MMC.cardType();
    if(cardType == CARD_NONE){
        Serial.println("No SD card attached");
    } else {
        Serial.print("SD Card Type: ");
        if(cardType == CARD_MMC){
            Serial.println("MMC");
        } else if(cardType == CARD_SD){
            Serial.println("SDSC");
        } else if(cardType == CARD_SDHC){
            Serial.println("SDHC");
        } else {
            Serial.println("UNKNOWN");
        }
        uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
        Serial.printf("SD Card Size: %lluMB\n", cardSize);
        
        // 打印一级目录内容
        // listDir(SD_MMC, "/", 0);
    }
  }

  // 初始化串口输出 FreeRTOS 任务
  initTasks();

  // 初始化定时器和DAC
  initDACoutput();

  DRAW_SetMode(DRAW_MODE_DMA);
  // 示例：使用新的绘图 API
  DRAW_Clear();
  // 画一个边框
  DRAW_AddRect(100, 100, 1800, 1800);
  // 画一个 X
  //DRAW_AddLine(100, 100, 1900, 1900);
  //DRAW_AddLine(100, 1900, 1900, 100);
  
  // 显示文字
  // 坐标 (200, 1000), 缩放 50 (字符高度约 7*50 = 350)
  DRAW_AddString("HELLO ESP32", 0, 200, 1000, 25, 25);
  
  // 启用 DMA 模式
  DRAW_SetMode(DRAW_MODE_DMA);
  
  // 设置 DAC 频率
  // 注意：由于 DMA 缓冲区在 PSRAM 中，访问速度较慢。
  // 且 sendDAC 使用软件模拟 SPI，开销较大。
  // 80kHz (12.5us) 会导致 CPU 100% 占用，饿死主线程。
  // 降低到 30kHz (33us) 以释放 CPU 资源。
  setDACFreq(30000); 

  
  // 提交帧
  DRAW_Render();

  // Initialize Terminal
DRAW_Terminal_Init(10, 100); // Scale 10%, Spacing 100 units
DRAW_Terminal_Print("SYSTEM BOOT...");
DRAW_Terminal_Print("CHECKING RAM...");

  Serial.println("Hardware initialization complete. Serial output task started.");
}

void loop() {
  // Main loop is empty, UI handled by FreeRTOS task
  delay(1000); 
}
