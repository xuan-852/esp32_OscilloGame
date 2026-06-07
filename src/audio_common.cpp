#include "audio_common.h"

// 音频缓冲区，用于存储音频样本数据
volatile uint16_t audioBuffer[AUDIO_BUFFER_SIZE];

// 缓冲区头指针，指向下一个要写入的位置
volatile uint32_t bufferHead = 0;

// 缓冲区尾指针，指向下一个要读取的位置
volatile uint32_t bufferTail = 0;