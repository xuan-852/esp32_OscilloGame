#include <Arduino.h>
#include "pins.h"
#include "DACoutput.h"
#include "vector_draw.h"
#include "driver/spi_master.h"

hw_timer_t * timer = NULL;

// Define constants from DAC8554.cpp since they are not in .h
#define DAC8554_BUFFER_WRITE          0x00
#define DAC8554_ALL_WRITE             0x20

// ESP-IDF SPI 相关变量
spi_device_handle_t spi;

// DAC8554 命令寄存器定义（与底层驱动保持一致）
#define DAC8554_BUFFER_WRITE          0x00
#define DAC8554_SINGLE_WRITE          0x10
#define DAC8554_ALL_WRITE             0x20
#define DAC8554_BROADCAST             0x30

/**
 * @brief 使用底层ESP-IDF SPI API发送DAC8554数据帧
 * @param channel DAC通道 (0-3)
 * @param value 16位DAC值 (0-65535)
 * @param writeMode 写入模式 (BUFFER_WRITE, SINGLE_WRITE, ALL_WRITE)
 */
void dac8554_send_frame(uint8_t channel, uint16_t value, uint8_t writeMode) {
    // 配置寄存器格式: [A1 A0 0 C1 C0 0 0 0] + 写入模式
    uint8_t configRegister = (channel << 1) | writeMode;
    
    // 准备SPI传输数据 (24位: 8位配置 + 16位数据)
    uint8_t tx_data[3];
    tx_data[0] = configRegister;
    tx_data[1] = (value >> 8) & 0xFF;  // 数据高8位
    tx_data[2] = value & 0xFF;         // 数据低8位
    
    // 配置SPI传输
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 24;          // 24位数据长度
    t.tx_buffer = tx_data;  // 发送缓冲区
    t.user = (void*)0;      // 用户数据
    
    // 执行SPI传输
    spi_device_polling_transmit(spi, &t);
    
    // 重要：拉低LDAC引脚以更新DAC输出
    digitalWrite(DAC_LDAC, LOW);
    delayMicroseconds(1);   // 保持至少25ns（手册要求）
    digitalWrite(DAC_LDAC, HIGH);
}

/**
 * @brief 初始化ESP-IDF SPI总线
 */
void init_esp_spi() {
    esp_err_t ret;
    
    // SPI总线配置 - 增大DMA传输大小限制
    spi_bus_config_t buscfg = {
        .mosi_io_num = DAC_MOSI,
        .miso_io_num = DAC_MISO,
        .sclk_io_num = DAC_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096  // 增大到4KB，支持大块DMA传输
    };
    
    // 初始化SPI总线 - 启用DMA
    ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        Serial.println("SPI总线初始化失败!");
        return;
    }
    
    // SPI设备配置
    spi_device_interface_config_t devcfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = 1,                    // SPI模式1 (CPOL=0, CPHA=1)
        .duty_cycle_pos = 128,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .clock_speed_hz = 20000000,   // 10MHz SPI时钟
        .input_delay_ns = 0,
        .spics_io_num = DAC_CS,       // CS引脚
        .flags = 0,
        .queue_size = 1,
        .pre_cb = NULL,
        .post_cb = NULL
    };
    
    // 添加SPI设备
    ret = spi_bus_add_device(SPI3_HOST, &devcfg, &spi);
    if (ret != ESP_OK) {
        Serial.println("SPI设备添加失败!");
        return;
    }
    
    Serial.println("ESP-IDF SPI初始化完成!");
}
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
  uint16_t outX, outY;
  
  // 获取当前需要绘制的坐标
  DRAW_GetNextPoint(outX, outY);

  // 发送 X 到通道 0
  dac8554_send_frame(0, outX, DAC8554_SINGLE_WRITE);

  // 发送 Y 到通道 1
  //dac8554_send_frame(1, outY, DAC8554_BUFFER_WRITE);

  // 脉冲 LDAC 更新输出
  GPIO.out_w1tc = ldacMask; // LOW
  GPIO.out_w1ts = ldacMask; // HIGH
}

void initDACoutput() {
  // 初始化 DAC LDAC 引脚
  pinMode(DAC_LDAC, OUTPUT);
  digitalWrite(DAC_LDAC, HIGH); 

  // 确保 DAC 初始化
 init_esp_spi();

  // 设置快速 GPIO 掩码
  csMask = (1 << DAC_CS);
  mosiMask = (1 << DAC_MOSI);
  sclkMask = (1 << DAC_SCLK);
  ldacMask = (1 << DAC_LDAC);

  // 初始化绘图逻辑
  DRAW_Init();

  // 初始化定时器 0
  // 预分频 2 -> 1 tick = 25ns (80MHz APB)
  timer = timerBegin(0, 2, true);

  // 绑定中断
  timerAttachInterrupt(timer, &onTimer, true);

  // 设置默认频率 80kHz
  setDACFreq(40000);

  // 启用报警
  timerAlarmEnable(timer);
}

void setDACFreq(uint32_t freq) {
  if (timer == NULL) return;
  if (freq == 0) freq = 1;
  
  // 80MHz / 2 = 40MHz
  // ticks = 40,000,000 / freq
  uint32_t ticks = 40000000 / freq;
  if (ticks < 100) ticks = 100; // 限制最大频率约 400kHz
  
  timerAlarmWrite(timer, ticks, true);
}
