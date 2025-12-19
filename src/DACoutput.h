#pragma once
#include <Arduino.h>

void initDACoutput();
void IRAM_ATTR sendDAC(uint8_t configRegister, uint16_t value);

// 设置 DAC 输出频率 (Hz)
// 默认约 80000Hz
void setDACFreq(uint32_t freq);

void Set_Audio_Mode(bool enable);

// 双缓冲接口
void Init_Audio_Buffers(); // 初始化 PSRAM 缓冲区
bool Is_Buf_A_Free();      // Buffer A 是否空闲（可写入）
bool Is_Buf_B_Free();      // Buffer B 是否空闲（可写入）
uint16_t* Get_Buf_A_L();   // 获取 Buffer A 左声道指针
uint16_t* Get_Buf_A_R();   // 获取 Buffer A 右声道指针
uint16_t* Get_Buf_A_X();   // 获取 Buffer A X指针
uint16_t* Get_Buf_A_Y();   // 获取 Buffer A Y指针
uint16_t* Get_Buf_B_L();   // 获取 Buffer B 左声道指针
uint16_t* Get_Buf_B_R();   // 获取 Buffer B 右声道指针
uint16_t* Get_Buf_B_X();   // 获取 Buffer B X指针
uint16_t* Get_Buf_B_Y();   // 获取 Buffer B Y指针
void Mark_Buf_A_Ready(int sample_count); // 标记 Buffer A 已填充完毕，供 ISR 播放
void Mark_Buf_B_Ready(int sample_count); // 标记 Buffer B 已填充完毕，供 ISR 播放

// Player Control
// 0: Vector (Default), 1: Audio (2ch), 2: Video (4ch)
void Set_Player_Mode(int mode);
