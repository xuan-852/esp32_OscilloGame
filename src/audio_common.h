#pragma once
#include <Arduino.h>

#define AUDIO_BUFFER_SIZE 8192 // 8KB buffer

extern volatile uint16_t audioBuffer[AUDIO_BUFFER_SIZE];
extern volatile uint32_t bufferHead;
extern volatile uint32_t bufferTail;
