#pragma once
#ifndef VOICE_CONTROL_H
#define VOICE_CONTROL_H

#include <Arduino.h>

// 支持的语音动作类型
// 对应 DeepSeek JSON 响应中的 "action" 字段
typedef enum {
    VC_NONE = 0,        // 无动作（纯对话）
    VC_OPEN_MUSIC,      // 打开音乐菜单
    VC_OPEN_VIDEO,      // 打开视频菜单
    VC_OPEN_GAMES,      // 打开游戏菜单
    VC_OPEN_ONLINE,     // 打开在线菜单
    VC_OPEN_GAME_JOY,   // 打开手柄模式
    VC_OPEN_AI_CHAT,    // 打开 AI 对话
    VC_OPEN_ABOUT,      // 打开关于
    VC_START_SNAKE,     // 启动贪吃蛇
    VC_START_BREAKOUT,  // 启动打砖块
    VC_START_FLAPPY,    // 启动 Flappy Bird
    VC_START_RACING,    // 启动赛车
    VC_START_RUNTINY,   // 启动 RunTiny
    VC_START_TANK,      // 启动坦克大战
    VC_BACK,            // 返回上级菜单
    VC_EXIT             // 退出到主菜单
} VC_Action;

// ---- 跨任务接口 ----
// ai_chat_task (Core 0) 设置这些变量
// guiTask (Core 1) 消费执行

// 当前待执行的语音动作
extern volatile VC_Action voice_action;
// 是否有待执行的动作（guiTask 消费后清零）
extern volatile bool voice_pending;

/**
 * @brief 解析 DeepSeek 返回的 JSON 字符串
 *
 * 期望格式: {"reply":"...","action":"action_name"}
 * - reply: 显示在终端上的文字
 * - action: 要执行的动作（见 VC_Action 枚举）
 *
 * 若解析成功且 action 非空，设置 voice_action 和 voice_pending。
 * 若解析失败，视作纯文本对话，不触发动作。
 *
 * @param json_reply DeepSeek 返回的原始字符串（可能是纯文本或 JSON）
 * @return String 要显示在终端上的回复文字
 */
String VC_ParseReply(const String& json_reply);

#endif // VOICE_CONTROL_H
