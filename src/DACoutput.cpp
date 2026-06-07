#include <Arduino.h>
#include "DAC8554.h"
#include "pins.h"
#include "DACoutput.h"
#include "vector_draw.h"

// 访问在main.cpp中定义的dac对象
extern DAC8554 dac;

hw_timer_t * timer = NULL;

// 从DAC8554.cpp定义常量，因为它们不在.h文件中
#define DAC8554_BUFFER_WRITE          0x00
#define DAC8554_ALL_WRITE             0x20

// 预计算的快速GPIO掩码
uint32_t csMask;
uint32_t mosiMask;
uint32_t sclkMask;
uint32_t ldacMask;

// 音频全局变量 - 双缓冲
// 每个缓冲区的最大样本数：64KB（单声道）= 32768个样本
// 为每个缓冲区分配32768个样本
#define MAX_SAMPLES_PER_BUF 32768

volatile uint16_t *bufA_L = NULL;
volatile uint16_t *bufA_R = NULL;
volatile uint16_t *bufA_X = NULL;
volatile uint16_t *bufA_Y = NULL;
volatile uint16_t *bufB_L = NULL;
volatile uint16_t *bufB_R = NULL;
volatile uint16_t *bufB_X = NULL;
volatile uint16_t *bufB_Y = NULL;

volatile bool bufA_ready = false; // 如果A有数据要播放则为true
volatile bool bufB_ready = false; // 如果B有数据要播放则为true
volatile int bufA_count = 0;
volatile int bufB_count = 0;

volatile bool playing_A = true; // 当前正在播放A为true，播放B为false
volatile int play_idx = 0;

volatile int player_mode = 0; // 0: 矢量模式, 1: 音频模式, 2: 视频模式

// 初始化音频缓冲区
void Init_Audio_Buffers() {
    if(bufA_L == NULL) {
        // 为双缓冲分配PSRAM内存
        bufA_L = (volatile uint16_t*)ps_malloc(MAX_SAMPLES_PER_BUF * sizeof(uint16_t));
        bufA_R = (volatile uint16_t*)ps_malloc(MAX_SAMPLES_PER_BUF * sizeof(uint16_t));
        bufA_X = (volatile uint16_t*)ps_malloc(MAX_SAMPLES_PER_BUF * sizeof(uint16_t));
        bufA_Y = (volatile uint16_t*)ps_malloc(MAX_SAMPLES_PER_BUF * sizeof(uint16_t));
        
        bufB_L = (volatile uint16_t*)ps_malloc(MAX_SAMPLES_PER_BUF * sizeof(uint16_t));
        bufB_R = (volatile uint16_t*)ps_malloc(MAX_SAMPLES_PER_BUF * sizeof(uint16_t));
        bufB_X = (volatile uint16_t*)ps_malloc(MAX_SAMPLES_PER_BUF * sizeof(uint16_t));
        bufB_Y = (volatile uint16_t*)ps_malloc(MAX_SAMPLES_PER_BUF * sizeof(uint16_t));
        
        if(!bufA_L || !bufA_R || !bufA_X || !bufA_Y) {
            Serial.println("为双缓冲区分配PSRAM失败！");
        } else {
            Serial.printf("在PSRAM中分配双缓冲区：每个缓冲区%d个样本\n", MAX_SAMPLES_PER_BUF);
        }
    }
    // 重置状态
    bufA_ready = false;
    bufB_ready = false;
    playing_A = true;
    play_idx = 0;
}

// --- 双缓冲接口 ---

// 检查缓冲区A是否空闲
bool Is_Buf_A_Free() { return !bufA_ready; }

// 检查缓冲区B是否空闲
bool Is_Buf_B_Free() { return !bufB_ready; }

// 获取缓冲区A的左声道指针
uint16_t* Get_Buf_A_L() { return (uint16_t*)bufA_L; }

// 获取缓冲区A的右声道指针
uint16_t* Get_Buf_A_R() { return (uint16_t*)bufA_R; }

// 获取缓冲区A的X坐标指针
uint16_t* Get_Buf_A_X() { return (uint16_t*)bufA_X; }

// 获取缓冲区A的Y坐标指针
uint16_t* Get_Buf_A_Y() { return (uint16_t*)bufA_Y; }

// 获取缓冲区B的左声道指针
uint16_t* Get_Buf_B_L() { return (uint16_t*)bufB_L; }

// 获取缓冲区B的右声道指针
uint16_t* Get_Buf_B_R() { return (uint16_t*)bufB_R; }

// 获取缓冲区B的X坐标指针
uint16_t* Get_Buf_B_X() { return (uint16_t*)bufB_X; }

// 获取缓冲区B的Y坐标指针
uint16_t* Get_Buf_B_Y() { return (uint16_t*)bufB_Y; }

// 标记缓冲区A准备就绪
void Mark_Buf_A_Ready(int count) {
    bufA_count = count;
    bufA_ready = true;
}

// 标记缓冲区B准备就绪
void Mark_Buf_B_Ready(int count) {
    bufB_count = count;
    bufB_ready = true;
}

// 设置播放器模式
void Set_Player_Mode(int mode) {
    player_mode = mode;
    if(mode > 0) {
        setDACFreq(44100);
        // 进入音频/视频模式时重置播放状态
        play_idx = 0;
        playing_A = true;
        bufA_ready = false;
        bufB_ready = false;
    } else {
        setDACFreq(80000); // 恢复矢量频率
    }
}

// 优化的DAC8554发送函数
// 使用硬件SPI发送24位数据
void IRAM_ATTR sendDAC(uint8_t configRegister, uint16_t value) {
  // CS引脚置低
  GPIO.out_w1tc = csMask;

  // 组合24位数据：8位配置寄存器 + 16位数值
  // 使用SPI.writeBytes发送缓冲区，通常比单字节传输更快且安全
  uint8_t data[3];
  data[0] = configRegister;
  data[1] = (value >> 8);
  data[2] = (value & 0xFF);
  
  SPI.writeBytes(data, 3);

  // CS引脚置高
  GPIO.out_w1ts = csMask;
}

// 定时器中断处理函数
void IRAM_ATTR onTimer() {
  if (player_mode > 0) {
      // 音频/视频模式
      uint16_t l = 32768;
      uint16_t r = 32768;
      uint16_t x = 32768;
      uint16_t y = 32768;
      bool has_data = false;

      if (playing_A) {
          if (bufA_ready) {
              l = bufA_L[play_idx];
              r = bufA_R[play_idx];
              if(player_mode == 2) {
                  x = bufA_X[play_idx];
                  y = bufA_Y[play_idx];
              }
              play_idx++;
              has_data = true;
              if (play_idx >= bufA_count) {
                  bufA_ready = false; // A缓冲区播放完成
                  playing_A = false;  // 切换到B缓冲区
                  play_idx = 0;
              }
          }
      } else {
          if (bufB_ready) {
              l = bufB_L[play_idx];
              r = bufB_R[play_idx];
              if(player_mode == 2) {
                  x = bufB_X[play_idx];
                  y = bufB_Y[play_idx];
              }
              play_idx++;
              has_data = true;
              if (play_idx >= bufB_count) {
                  bufB_ready = false; // B缓冲区播放完成
                  playing_A = true;   // 切换到A缓冲区
                  play_idx = 0;
              }
          }
      }

      if (has_data) {
          // 发送音频到通道2和3
          sendDAC(DAC8554_BUFFER_WRITE | (2 << 1), l);
          sendDAC(DAC8554_BUFFER_WRITE | (3 << 1), r);
          
          if(player_mode == 2) {
              // 发送视频到通道0和1
              sendDAC(DAC8554_BUFFER_WRITE | (0 << 1), x);
              sendDAC(DAC8554_BUFFER_WRITE | (1 << 1), y);
          }
          
          // 更新DAC输出
          GPIO.out_w1tc = ldacMask; 
          GPIO.out_w1ts = ldacMask; 
      }
  } else {
      // 矢量模式
      uint16_t outX, outY;
      
      // 获取当前需要绘制的坐标
      DRAW_GetNextPoint(outX, outY);

      // 发送X坐标到通道0
      sendDAC(DAC8554_BUFFER_WRITE | (0 << 1), outX);

      // 发送Y坐标到通道1
      sendDAC(DAC8554_BUFFER_WRITE | (1 << 1), outY);

      // 脉冲LDAC引脚更新输出
      GPIO.out_w1tc = ldacMask; // 置低
      GPIO.out_w1ts = ldacMask; // 置高
  }
}

// 初始化DAC输出系统
void initDACoutput() {
  // 初始化DAC LDAC引脚
  pinMode(DAC_LDAC, OUTPUT);
  digitalWrite(DAC_LDAC, HIGH); 
  
  // 初始化CS引脚
  pinMode(DAC_CS, OUTPUT);
  digitalWrite(DAC_CS, HIGH);

  // 初始化SPI通信
  SPI.begin(DAC_SCLK, DAC_MISO, DAC_MOSI, DAC_CS);
  SPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE1));

  // 设置快速GPIO掩码
  csMask = (1 << DAC_CS);
  mosiMask = (1 << DAC_MOSI);
  sclkMask = (1 << DAC_SCLK);
  ldacMask = (1 << DAC_LDAC);

  // 初始化绘图逻辑
  DRAW_Init();
  
  // 初始化音频缓冲区（PSRAM）
  Init_Audio_Buffers();

  // 初始化定时器0
  // 使用80分频 -> 1MHz（1微秒）
  // 这样兼容性更好，且对于音频（22微秒）精度足够
  timer = timerBegin(0, 80, true);

  // 绑定中断处理函数
  timerAttachInterrupt(timer, &onTimer, true);

  // 设置默认频率80kHz
  setDACFreq(80000);

  // 启用定时器报警
  timerAlarmEnable(timer);
}

// 设置DAC输出频率
void setDACFreq(uint32_t freq) {
  if (timer == NULL) return;
  if (freq == 0) freq = 1;
  
  // 基础时钟现在是1MHz（1,000,000 Hz）
  // 定时器计数 = 1,000,000 / 频率
  uint32_t ticks = 1000000 / freq;
  if (ticks < 2) ticks = 2; 
  
  timerAlarmWrite(timer, ticks, true);
}