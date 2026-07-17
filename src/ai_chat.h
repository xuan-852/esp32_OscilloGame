#pragma once

#ifndef AI_CHAT_H
#define AI_CHAT_H

#include <Arduino.h>

// AI Chat 是否正在运行
extern volatile bool ai_chat_active;

// AI Chat 阶段枚举 — 每个阶段对应不同的屏幕提示
// 数值必须与 freertos.cpp 中的 guiTask 一致
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

// 串口命令分发器 — 从 main.cpp loop() 中调用
void handle_test_commands();

// ========== 测试接口 (自动化测试用) ==========
extern volatile bool test_ai_enter;       // 告诉 guiTask 进入 AI Chat
extern volatile bool test_ai_exit;        // 告诉 guiTask 退出 AI Chat
extern volatile bool test_btn_pressed;    // 模拟 ENTER 按钮保持按下 (true=按下/LOW)
extern volatile bool test_btn_triggered;  // 模拟一次按钮点击 (脉冲，消费后清0)
extern volatile bool test_no_mic;         // true: 跳过 MIC 初始化 (纯链路测试)

#endif

