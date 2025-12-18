#include <Arduino.h>
#include "DAC8554.h"
#include "pins.h"
#include "DACoutput.h"
#include "vector_draw.h"

// Access the dac object defined in main.cpp
extern DAC8554 dac;

hw_timer_t * timer = NULL;

// Define constants from DAC8554.cpp since they are not in .h
#define DAC8554_BUFFER_WRITE          0x00
#define DAC8554_ALL_WRITE             0x20

// Pre-calculated masks for fast GPIO
uint32_t csMask;
uint32_t mosiMask;
uint32_t sclkMask;
uint32_t ldacMask;

// Audio Globals - Double Buffering
// Max samples per buffer: 64KB (Mono) = 32768 samples.
// Let's allocate 32768 samples per buffer.
#define MAX_SAMPLES_PER_BUF 32768

volatile uint16_t *bufA_L = NULL;
volatile uint16_t *bufA_R = NULL;
volatile uint16_t *bufB_L = NULL;
volatile uint16_t *bufB_R = NULL;

volatile bool bufA_ready = false; // True if A has data to play
volatile bool bufB_ready = false; // True if B has data to play
volatile int bufA_count = 0;
volatile int bufB_count = 0;

volatile bool playing_A = true; // True if currently playing A, False if B
volatile int play_idx = 0;

volatile bool audio_mode = false;

void Init_Audio_Buffers() {
    if(bufA_L == NULL) {
        bufA_L = (volatile uint16_t*)ps_malloc(MAX_SAMPLES_PER_BUF * sizeof(uint16_t));
        bufA_R = (volatile uint16_t*)ps_malloc(MAX_SAMPLES_PER_BUF * sizeof(uint16_t));
        bufB_L = (volatile uint16_t*)ps_malloc(MAX_SAMPLES_PER_BUF * sizeof(uint16_t));
        bufB_R = (volatile uint16_t*)ps_malloc(MAX_SAMPLES_PER_BUF * sizeof(uint16_t));
        
        if(!bufA_L || !bufA_R || !bufB_L || !bufB_R) {
            Serial.println("Failed to allocate PSRAM for Double Buffers!");
        } else {
            Serial.printf("Allocated Double Buffers in PSRAM: %d samples each\n", MAX_SAMPLES_PER_BUF);
        }
    }
    // Reset state
    bufA_ready = false;
    bufB_ready = false;
    playing_A = true;
    play_idx = 0;
}

// --- Double Buffer Interface ---
bool Is_Buf_A_Free() { return !bufA_ready; }
bool Is_Buf_B_Free() { return !bufB_ready; }
uint16_t* Get_Buf_A_L() { return (uint16_t*)bufA_L; }
uint16_t* Get_Buf_A_R() { return (uint16_t*)bufA_R; }
uint16_t* Get_Buf_B_L() { return (uint16_t*)bufB_L; }
uint16_t* Get_Buf_B_R() { return (uint16_t*)bufB_R; }

void Mark_Buf_A_Ready(int count) {
    bufA_count = count;
    bufA_ready = true;
}

void Mark_Buf_B_Ready(int count) {
    bufB_count = count;
    bufB_ready = true;
}

void Set_Audio_Mode(bool enable) {
    // Force recompile
    audio_mode = enable;
    if(enable) {
        setDACFreq(44100);
        // Reset playback state when entering audio mode
        play_idx = 0;
        playing_A = true;
        bufA_ready = false;
        bufB_ready = false;
    } else {
        setDACFreq(80000); // Restore video freq
    }
}

// 优化的 DAC8554 发送函数
// 使用硬件 SPI 发送 24位数据
void IRAM_ATTR sendDAC(uint8_t configRegister, uint16_t value) {
  // CS LOW
  GPIO.out_w1tc = csMask;

  // 组合 24位 数据: 8位 Config + 16位 Value
  // 使用 SPI.writeBytes 发送 buffer，这通常比单字节 transfer 更快且安全
  uint8_t data[3];
  data[0] = configRegister;
  data[1] = (value >> 8);
  data[2] = (value & 0xFF);
  
  SPI.writeBytes(data, 3);

  // CS HIGH
  GPIO.out_w1ts = csMask;
}

void IRAM_ATTR onTimer() {
  if (audio_mode) {
      uint16_t l = 32768; // Midpoint silence
      uint16_t r = 32768;
      bool has_data = false;

      if (playing_A) {
          if (bufA_ready) {
              l = bufA_L[play_idx];
              r = bufA_R[play_idx];
              play_idx++;
              has_data = true;
              if (play_idx >= bufA_count) {
                  bufA_ready = false; // Done with A
                  playing_A = false;  // Switch to B
                  play_idx = 0;
              }
          }
      } else {
          if (bufB_ready) {
              l = bufB_L[play_idx];
              r = bufB_R[play_idx];
              play_idx++;
              has_data = true;
              if (play_idx >= bufB_count) {
                  bufB_ready = false; // Done with B
                  playing_A = true;   // Switch to A
                  play_idx = 0;
              }
          }
      }

      if (has_data) {
          // Send to Ch 2 and 3
          sendDAC(DAC8554_BUFFER_WRITE | (2 << 1), l);
          sendDAC(DAC8554_BUFFER_WRITE | (3 << 1), r);
          
          // Update
          GPIO.out_w1tc = ldacMask; 
          GPIO.out_w1ts = ldacMask; 
      }
  } else {
      uint16_t outX, outY;
      
      // 获取当前需要绘制的坐标
      DRAW_GetNextPoint(outX, outY);

      // 发送 X 到通道 0
      sendDAC(DAC8554_BUFFER_WRITE | (0 << 1), outX);

      // 发送 Y 到通道 1
      sendDAC(DAC8554_BUFFER_WRITE | (1 << 1), outY);

      // 脉冲 LDAC 更新输出
      GPIO.out_w1tc = ldacMask; // LOW
      GPIO.out_w1ts = ldacMask; // HIGH
  }
}

void initDACoutput() {
  // 初始化 DAC LDAC 引脚
  pinMode(DAC_LDAC, OUTPUT);
  digitalWrite(DAC_LDAC, HIGH); 
  
  // 初始化 CS 引脚
  pinMode(DAC_CS, OUTPUT);
  digitalWrite(DAC_CS, HIGH);

  // 初始化 SPI
  SPI.begin(DAC_SCLK, DAC_MISO, DAC_MOSI, DAC_CS);
  SPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE1));

  // 设置快速 GPIO 掩码
  csMask = (1 << DAC_CS);
  mosiMask = (1 << DAC_MOSI);
  sclkMask = (1 << DAC_SCLK);
  ldacMask = (1 << DAC_LDAC);

  // 初始化绘图逻辑
  DRAW_Init();
  
  // 初始化音频缓冲区 (PSRAM)
  Init_Audio_Buffers();

  // 初始化定时器 0
  // 使用 80 分频 -> 1MHz (1us)
  // 这样兼容性更好，且对于音频 (22us) 精度足够
  timer = timerBegin(0, 80, true);

  // 绑定中断
  timerAttachInterrupt(timer, &onTimer, true);

  // 设置默认频率 80kHz
  setDACFreq(80000);

  // 启用报警
  timerAlarmEnable(timer);
}

void setDACFreq(uint32_t freq) {
  if (timer == NULL) return;
  if (freq == 0) freq = 1;
  
  // Base clock is now 1MHz (1,000,000 Hz)
  // ticks = 1,000,000 / freq
  uint32_t ticks = 1000000 / freq;
  if (ticks < 2) ticks = 2; 
  
  timerAlarmWrite(timer, ticks, true);
}
