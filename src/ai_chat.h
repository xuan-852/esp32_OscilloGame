#pragma once

#ifndef AI_CHAT_H
#define AI_CHAT_H

#include <Arduino.h>

// AI Chat state
extern volatile bool ai_chat_active;

// 阶段式显示 — 每个阶段显示对应提示（数值必须与 freertos.cpp 一致）
enum AIChatPhase {
    AI_PHASE_IDLE       = 0,
    AI_PHASE_CONNECTING = 1,
    AI_PHASE_TOKEN      = 2,
    AI_PHASE_WAITING    = 3,
    AI_PHASE_RECORDING  = 4,
    AI_PHASE_ASR        = 5,
    AI_PHASE_THINKING   = 6,
    AI_PHASE_REPLY      = 7,
    AI_PHASE_DONE       = 8,
    AI_PHASE_ERROR      = 9,
};
extern volatile AIChatPhase ai_chat_phase;
extern char  ai_chat_display_text[2048];
extern volatile bool ai_chat_dirty;

#define AI_SCALE        30
#define AI_SPACING      0
#define AI_CENTER_X     800
#define AI_CENTER_Y     1024
#define AI_REPLY_START_Y 1024
#define AI_REPLY_SPACING 200

void AI_Chat_Start();
void AI_Chat_Stop();

// 连续对话模式：设为 true 后新任务跳过 WAITING 直接进录音
extern volatile bool ai_continue_mode;

#endif

