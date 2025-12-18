#include "audio_common.h"

volatile uint16_t audioBuffer[AUDIO_BUFFER_SIZE];
volatile uint32_t bufferHead = 0;
volatile uint32_t bufferTail = 0;
