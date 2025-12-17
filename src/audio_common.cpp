#include "audio_common.h"

volatile uint32_t audioBuffer[AUDIO_BUFFER_SIZE];
volatile uint32_t bufferHead = 0;
volatile uint32_t bufferTail = 0;
volatile bool audioPlaying = false;
volatile uint8_t audioVolume = 50; // Default 50%
