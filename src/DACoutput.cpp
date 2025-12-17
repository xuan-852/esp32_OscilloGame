#include <Arduino.h>
#include "DAC8554.h"
#include "pins.h"
#include "DACoutput.h"
#include "vector_draw.h"
#include "audio_common.h"

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

// 优化的 DAC8554 发送函数
// 合并 Config 和 Value 为 24位数据发送，减少循环开销
void IRAM_ATTR sendDAC(uint8_t configRegister, uint16_t value) {
  // CS LOW
  GPIO.out_w1tc = csMask;

  // 组合 24位 数据: 8位 Config + 16位 Value
  uint32_t data = ((uint32_t)configRegister << 16) | value;

  // 发送 24 bits
  for (uint32_t mask = 0x800000; mask; mask >>= 1) {
    if (data & mask) GPIO.out_w1ts = mosiMask;
    else GPIO.out_w1tc = mosiMask;
    
    GPIO.out_w1ts = sclkMask;
    GPIO.out_w1tc = sclkMask;
  }

  // CS HIGH
  GPIO.out_w1ts = csMask;
}

void IRAM_ATTR onTimer() {
  static bool flip = false;
  flip = !flip;

  uint16_t outX, outY;
  
  // Audio Output (Channels 2 & 3) - Priority High
  // Update every tick for 44.1kHz
  if (audioPlaying && bufferHead != bufferTail) {
      uint32_t sample = audioBuffer[bufferTail];
      bufferTail = (bufferTail + 1) % AUDIO_BUFFER_SIZE;
      
      uint16_t ch2 = sample & 0xFFFF;
      uint16_t ch3 = (sample >> 16) & 0xFFFF;
      
      sendDAC(DAC8554_BUFFER_WRITE | (2 << 1), ch2);
      sendDAC(DAC8554_BUFFER_WRITE | (3 << 1), ch3);
  }

  // Vector Output (Channels 0 & 1)
  // If audio is playing, DISABLE vector output to save ISR time for audio
  if (!audioPlaying) {
      // 获取当前需要绘制的坐标
      DRAW_GetNextPoint(outX, outY);

      // 发送 X 到通道 0
      sendDAC(DAC8554_BUFFER_WRITE | (0 << 1), outX);

      // 发送 Y 到通道 1
      sendDAC(DAC8554_BUFFER_WRITE | (1 << 1), outY);
  }

  // 脉冲 LDAC 更新输出
  GPIO.out_w1tc = ldacMask; // LOW
  GPIO.out_w1ts = ldacMask; // HIGH
}

void initDACoutput() {
  // 初始化 DAC LDAC 引脚
  pinMode(DAC_LDAC, OUTPUT);
  digitalWrite(DAC_LDAC, HIGH); 

  // 确保 DAC 初始化
  dac.begin();

  // 设置快速 GPIO 掩码
  csMask = (1 << DAC_CS);
  mosiMask = (1 << DAC_MOSI);
  sclkMask = (1 << DAC_SCLK);
  ldacMask = (1 << DAC_LDAC);

  // 初始化绘图逻辑
  DRAW_Init();

  // 初始化定时器 0
  // Use prescaler 80 to get 1MHz timer (1us per tick) assuming 80MHz APB
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
  
  // Timer is 1MHz (1us per tick)
  // ticks = 1,000,000 / freq
  uint32_t ticks = 1000000 / freq;
  if (ticks < 2) ticks = 2; 
  
  timerAlarmWrite(timer, ticks, true);
}
