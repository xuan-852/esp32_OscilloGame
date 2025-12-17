#pragma once
#include <Arduino.h>

// Buffer size in frames (1 frame = 1 sample for Ch2 + 1 sample for Ch3)
// Using uint32_t to store packed L/R samples (High=Ch3, Low=Ch2)
#define AUDIO_BUFFER_SIZE 16384 

extern volatile uint32_t audioBuffer[AUDIO_BUFFER_SIZE];
extern volatile uint32_t bufferHead;
extern volatile uint32_t bufferTail;
extern volatile bool audioPlaying;
extern volatile uint8_t audioVolume; // 0-100

// Helper to check buffer fullness
static inline uint32_t audioBufferCount() {
    if (bufferHead >= bufferTail) return bufferHead - bufferTail;
    return AUDIO_BUFFER_SIZE - bufferTail + bufferHead;
}

static inline bool audioBufferFull() {
    return ((bufferHead + 1) % AUDIO_BUFFER_SIZE) == bufferTail;
}
