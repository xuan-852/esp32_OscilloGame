/**
 * @file voice_control.cpp
 * @brief 语音动作解析 — 解析 DeepSeek JSON 响应
 */
#include "voice_control.h"
#include <ArduinoJson.h>

volatile VC_Action voice_action = VC_NONE;
volatile bool voice_pending = false;

// 动作名称 → 枚举映射表（必须与 VC_Action 顺序一致）
static const char* action_names[] = {
    "open_music", "open_video", "open_games", "open_online",
    "open_game_joy", "open_ai_chat", "open_about",
    "start_snake", "start_breakout", "start_flappy",
    "start_racing", "start_runtiny", "start_tank",
    "back", "exit"
};
static const int action_count = sizeof(action_names) / sizeof(action_names[0]);

String VC_ParseReply(const String& json_reply) {
    voice_action = VC_NONE;
    voice_pending = false;

    if (json_reply.length() == 0) return json_reply;

    // 尝试寻找 JSON 对象
    int brace_start = json_reply.indexOf('{');
    if (brace_start < 0) return json_reply; // 纯文本，无动作

    String json_part = json_reply.substring(brace_start);
    int brace_end = json_part.lastIndexOf('}');
    if (brace_end < 0) return json_reply;

    json_part = json_part.substring(0, brace_end + 1);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json_part);
    if (err) return json_reply; // 非 JSON，当作纯文本

    const char* reply_text = doc["reply"];
    const char* action_str = doc["action"];

    if (!action_str || strlen(action_str) == 0) {
        // 有 JSON 但无 action，返回 reply 文本
        if (reply_text) return String(reply_text);
        return json_reply;
    }

    // 匹配动作
    VC_Action action = VC_NONE;
    for (int i = 0; i < action_count; i++) {
        if (strcmp(action_str, action_names[i]) == 0) {
            action = (VC_Action)(i + 1); // VC_NONE=0, 动作从 1 开始
            break;
        }
    }

    if (action != VC_NONE) {
        voice_action = action;
        voice_pending = true;
    }

    if (reply_text) return String(reply_text);
    return String(action_str);
}
