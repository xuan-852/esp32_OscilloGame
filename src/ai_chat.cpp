/**
 * @file ai_chat.cpp
 * @brief AI 对话全链路：录音(INMP441) → 百度 ASR → DeepSeek 大模型 → 示波器显示 + 动作跳转
 *
 * 引脚定义：MIC_SCK=47, MIC_WS=48, MIC_DATA=1（定义在 pins.h）
 *
 * 四大设计目标：
 *   1. 英文回复（矢量字库只支持 ASCII）
 *   2. 速率优先（deepseek-v4-flash + 非流式 + HTTP/1.0）
 *   3. JSON 动作控制 AI 跳转（打开游戏/音乐/视频等）
 *   4. 每阶段屏幕提示（等待/录音/识别/思考/回复/错误）
 */

#include "ai_chat.h"
#include "pins.h"
#include "microphone.h"
#include "freertos.h"
#include "web_server.h"
#include "vector_draw.h"
#include "DACoutput.h"
#include "voice_control.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>     // heap_caps_get_largest_free_block
#include "ai_config.h"


#define SAMPLE_RATE      16000
#define MAX_RECORD_SEC   10
#define CHUNK_SIZE       512

volatile bool ai_chat_active = false;

static Microphone* s_mic = nullptr;
static char baidu_token[256] = {0};
static unsigned long token_expires = 0;
static TaskHandle_t s_aiChatTaskHandle = NULL;
static SemaphoreHandle_t s_aiChatSemaphore = NULL;  // 唤醒持久任务的信号量
// 注意：WiFiClientSecure + HTTPClient 现在使用局部对象（每轮对话创建）
// 因为 DeepSeek 服务器主动发送 Connection: close，每轮 SSL 必然断开
// 全局保活无意义，改用局部对象避免 end()/begin() 循环脏状态

static bool     wifi_connect();
static bool     wifi_ensure();
static bool     baidu_get_token();
static String   baidu_asr(const int16_t* pcm, size_t samples);
static String   deepseek_chat(const String& text);
static void     ai_chat_task(void* pvParameters);

// ---- 阶段提示文本映射 ----
static const char* phase_text(AIChatPhase phase) {
    switch (phase) {
        case AI_PHASE_CONNECTING: return "Connecting WiFi...";
        case AI_PHASE_TOKEN:      return "Getting token...";
        case AI_PHASE_WAITING:    return "Press ENTER to record";
        case AI_PHASE_RECORDING:  return "Recording... release to stop";
        case AI_PHASE_ASR:        return "Recognizing...";
        case AI_PHASE_THINKING:   return "Thinking...";
        case AI_PHASE_DONE:       return "Press ENTER to exit";
        case AI_PHASE_ERROR:      return "Error! Check Serial";
        default:                  return "";
    }
}

void AI_Chat_Start() {
    // SSL/HTTP 单例 — 全局静态对象永远不析构。退出重进时，SSL/TCP 连接保持存活。
    // ★ A1 Task 永活方案：首次创建持久任务+信号量，后续只给信号量唤醒。
    //   永不删除任务 → 栈只分配一次 → 零碎片、零回收开销。

    // 防重入：如果任务正在运行（已有信号量在执行中），忽略本次请求
    if (ai_chat_active) {
        Serial.println("[AICHAT] already running, ignoring Start");
        return;
    }

    // 清除可能残留的语音动作（防止上一轮未消费）
    voice_pending = false;
    voice_action = VC_NONE;
    // 防止 WiFi 省电中断 HTTPS
    WiFi.setSleep(false);
    delay(500);   // ★ 等待射频完全恢复

    // 检测是否可跳过 init 阶段：WiFi 已连 + Token 缓存
    // 注意：不检查 SSL 存活 — DeepSeek 服务器主动 Connection:close，每轮新连
    bool wifi_ok   = (WiFi.status() == WL_CONNECTED);
    bool token_ok  = (strlen(baidu_token) > 0 && millis() / 1000 < token_expires);

    if (wifi_ok && token_ok) {
        strncpy(ai_chat_display_text, phase_text(AI_PHASE_WAITING), sizeof(ai_chat_display_text) - 1);
        ai_chat_display_text[sizeof(ai_chat_display_text) - 1] = '\0';
        ai_chat_phase = AI_PHASE_WAITING;
        Serial.println("[AICHAT] SSL alive, skipping init display");
    } else {
        strncpy(ai_chat_display_text, phase_text(AI_PHASE_CONNECTING), sizeof(ai_chat_display_text) - 1);
        ai_chat_display_text[sizeof(ai_chat_display_text) - 1] = '\0';
        ai_chat_phase = AI_PHASE_CONNECTING;
        Serial.println("[AICHAT] init display: CONNECTING");
    }
    ai_chat_dirty = true;

    // 首次：创建持久任务和信号量
    if (s_aiChatTaskHandle == NULL) {
        Serial.printf("[AICHAT] creating persistent task, heap=%u\n", ESP.getFreeHeap());
        if (xTaskCreatePinnedToCore(ai_chat_task, "AIChatTask", 8192, NULL, 1, &s_aiChatTaskHandle, 0) != pdPASS) {
            Serial.printf("[AICHAT] FAILED to create task! heap=%u\n", ESP.getFreeHeap());
            ai_chat_active = false;
            s_aiChatTaskHandle = NULL;
            ai_chat_phase = AI_PHASE_IDLE;
            ai_chat_display_text[0] = '\0';
            ai_chat_dirty = true;
            return;
        }
        if (s_aiChatSemaphore == NULL) {
            s_aiChatSemaphore = xSemaphoreCreateBinary();
        }
    }

    // 激活并唤醒任务
    ai_chat_active = true;
    xSemaphoreGive(s_aiChatSemaphore);
    Serial.println("[AICHAT] semaphore given, task will start conversation");
}

void AI_Chat_Stop() {
    ai_chat_active = false;
    // 给信号量让任务从等待/阻塞中唤醒，进入 done 清理后回到 idle
    if (s_aiChatSemaphore != NULL) {
        xSemaphoreGive(s_aiChatSemaphore);
    }
}

// ---- 共享变量（供 guiTask 读取渲染） ----
char  ai_chat_display_text[2048];
volatile AIChatPhase ai_chat_phase = AI_PHASE_IDLE;
volatile bool ai_chat_dirty = false;

// ========== 测试接口 (自动化测试用) ==========
volatile bool test_ai_enter      = false;
volatile bool test_ai_exit       = false;
volatile bool test_btn_pressed   = false;  // true=按下 ENTER
volatile bool test_btn_triggered = false;  // 单次点击
volatile bool test_no_mic        = false;  // 跳过 MIC 初始化

// 串口命令分发器 — 从 main.cpp loop() 中调用
void handle_test_commands() {
    if (!Serial.available()) return;
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "test:enter")       { test_ai_enter = true;      Serial.println("[TEST] set test_ai_enter"); }
    else if (cmd == "test:exit")   { test_ai_exit = true;       Serial.println("[TEST] set test_ai_exit"); }
    else if (cmd == "test:btn")    { test_btn_triggered = true; Serial.println("[TEST] set test_btn_triggered"); }
    else if (cmd == "test:btn_down") { test_btn_pressed = true; Serial.println("[TEST] set test_btn_pressed=1"); }
    else if (cmd == "test:btn_up")   { test_btn_pressed = false;Serial.println("[TEST] set test_btn_pressed=0"); }
    else if (cmd == "test:no_mic") { test_no_mic = true;       Serial.println("[TEST] set test_no_mic"); }
    else if (cmd == "test:mic")    { test_no_mic = false;      Serial.println("[TEST] set test_no_mic=0"); }
    else if (cmd.length() > 0)     { Serial.printf("[TEST] unknown: \"%s\"\n", cmd.c_str()); }
}

void ai_show(AIChatPhase phase, const char* text) {
    if (text && strlen(text) > 0) {
        strncpy(ai_chat_display_text, text, sizeof(ai_chat_display_text) - 1);
    } else if (text == NULL) {
        // text=NULL → 只改阶段，保留已有文本
        // 用于 DONE 阶段不覆盖回复内容
    } else {
        strncpy(ai_chat_display_text, phase_text(phase), sizeof(ai_chat_display_text) - 1);
    }
    ai_chat_display_text[sizeof(ai_chat_display_text) - 1] = '\0';
    ai_chat_phase = phase;
    ai_chat_dirty = true;
}

// ---- WiFi ----
static bool wifi_connect() {
    if (WiFi.status() == WL_CONNECTED) return true;
    ai_show(AI_PHASE_CONNECTING, "");

    Serial.println("[AICHAT] Connecting WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long t0 = millis();
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(200);
        Serial.print(".");
        if (++tries > 50) {
            Serial.println();
            Serial.println("[AICHAT] WiFi timeout!");
            ai_show(AI_PHASE_ERROR, "WiFi failed!");
            return false;
        }
    }
    Serial.printf("[AICHAT] WiFi OK, IP: %s (%ums)\n", WiFi.localIP().toString().c_str(), millis() - t0);
    return true;
}

static bool wifi_ensure() {
    if (WiFi.status() == WL_CONNECTED) return true;
    Serial.println("[AICHAT] WiFi lost! Reconnecting...");
    WiFi.reconnect();
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
        delay(200);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[AICHAT] WiFi recovered (%ums)\n", millis() - t0);
        return true;
    }
    Serial.println("[AICHAT] WiFi reconnect failed, full reconnect");
    WiFi.disconnect(true);
    delay(100);
    return wifi_connect();
}

// ---- 百度 Token（缓存刷新） ----
static bool baidu_get_token() {
    if (strlen(baidu_token) > 0 && millis() / 1000 < token_expires) return true;
    if (!wifi_ensure()) return false;

    ai_show(AI_PHASE_TOKEN, "");

    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) { delay(2000); if (!wifi_ensure()) return false; }

        WiFiClient client;
        HTTPClient http;
        String url = String("http://aip.baidubce.com/oauth/2.0/token")
                     + "?grant_type=client_credentials"
                     + "&client_id=" + BAIDU_API_KEY
                     + "&client_secret=" + BAIDU_SECRET_KEY;

        http.begin(client, url);
        http.setTimeout(10000);
        int code = http.GET();
        String resp = http.getString();
        http.end();

        if (code != 200) { Serial.printf("[AICHAT] Baidu token HTTP %d\n", code); continue; }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, resp);
        if (err) { Serial.printf("[AICHAT] Baidu token parse fail: %s\n", err.c_str()); continue; }

        const char* token = doc["access_token"];
        int expires_in = doc["expires_in"];
        if (!token) { Serial.println("[AICHAT] Baidu token empty!"); continue; }

        strncpy(baidu_token, token, sizeof(baidu_token) - 1);
        token_expires = millis() / 1000 + expires_in - 86400;
        Serial.println("[AICHAT] Baidu token OK");
        return true;
    }
    Serial.println("[AICHAT] Baidu token exhausted retries");
    return false;
}

// ---- 百度语音识别（ASR） ----
static String baidu_asr(const int16_t* pcm, size_t samples) {
    Serial.println("[AICHAT] baidu_get_token...");
    if (!baidu_get_token()) return String();
    size_t pcm_bytes = samples * sizeof(int16_t);
    if (pcm_bytes == 0) { Serial.println("[AICHAT] ASR: empty pcm"); return String(); }

    ai_show(AI_PHASE_ASR, "");
    Serial.printf("[AICHAT] ASR sending %u bytes...\n", pcm_bytes);

    WiFiClient client;
    HTTPClient http;
    String url = String("http://vop.baidu.com/pro_api")
               + "?cuid=ESP32_S3"
               + "&token=" + baidu_token
               + "&dev_pid=80001";

    http.begin(client, url);
    http.addHeader("Content-Type", "audio/pcm;rate=16000");
    http.setTimeout(30000);

    unsigned long t_asr = millis();
    int code = http.POST((uint8_t*)pcm, pcm_bytes);
    Serial.printf("[AICHAT] ASR HTTP %d (%ums)\n", code, millis() - t_asr);
    if (code != 200) { http.end(); client.stop(); return String(); }

    String resp = http.getString();
    http.end();
    client.stop();
    Serial.printf("[AICHAT] ASR response %u bytes\n", resp.length());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, resp);
    if (err) { Serial.printf("[AICHAT] ASR parse fail: %s\n", err.c_str()); return String(); }

    int err_no = doc["err_no"];
    if (err_no != 0) {
        Serial.printf("[AICHAT] ASR err_no: %d\n", err_no);
        return String();
    }

    JsonArray result = doc["result"].as<JsonArray>();
    if (result.size() == 0) { Serial.println("[AICHAT] ASR result empty"); return String(); }
    return result[0].as<String>();
}

// ---- DeepSeek 对话（局部对象，每轮全新 SSL 握手）
// 每轮对话创建全新 WiFiClientSecure + HTTPClient 局部对象。
// DeepSeek 服务器主动发送 Connection: close，SSL 连接每轮必然断开。
// 用局部对象避免全局复用时的 end()/begin() 循环脏状态。
// SSL 握手 ~800ms/次，换取绝对可靠的 body 读取。
static String deepseek_chat(const String& text) {
    // 确保 WiFi 在线
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[DSEEK] WiFi lost, reconnecting...");
        WiFi.reconnect();
        int w = 0;
        while (WiFi.status() != WL_CONNECTED && w < 40) { delay(250); w++; }
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[DSEEK] WiFi reconnect failed");
            return String();
        }
        Serial.println("[DSEEK] WiFi reconnected");
    }

// ---- 创建全新 SSL client + HTTPClient ----
    WiFiClientSecure client;
    HTTPClient http;
    client.setInsecure();
    http.setTimeout(30000);
    http.setConnectTimeout(10000);
    http.useHTTP10(true);
    http.begin(client, "https://api.deepseek.com/chat/completions");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + String(DEEPSEEK_API_KEY));
    Serial.printf("[DSEEK] heap: %u  WiFi RSSI: %d\n", ESP.getFreeHeap(), WiFi.RSSI());

// ---- 构造 JSON 请求体 ----
        JsonDocument req_doc;
        req_doc["model"] = "deepseek-v4-flash";
        req_doc["temperature"] = 0.7;
        req_doc["max_tokens"] = 500;
        req_doc["stream"] = false;
        req_doc["thinking"]["type"] = "disabled";

        JsonArray messages = req_doc["messages"].to<JsonArray>();
        JsonObject sys = messages.add<JsonObject>();
        sys["role"] = "system";
        sys["content"] =
            "You control an oscilloscope game console. Reply STRICTLY in ENGLISH ONLY. "
            "ABSOLUTELY NEVER use Chinese or any non-ASCII characters. "
            "You can write longer replies when needed, pure ASCII text only. "
            "For short casual chat just respond naturally.\n"
            "CRITICAL: NEVER wrap your reply in quotes, backticks, code blocks, "
            "or any formatting markers. Return ONLY the raw sentence text.\n"
            "For navigation actions, reply with JSON (no markdown):\n"
            "{\"reply\":\"...\",\"action\":\"action_name\"}\n"
            "Actions: open_music, open_video, open_games, open_online, "
            "open_game_joy, open_ai_chat, open_about, "
            "start_snake, start_breakout, start_flappy, start_racing, "
            "start_runtiny, start_tank, back, exit";

        JsonObject user = messages.add<JsonObject>();
        user["role"] = "user";
        user["content"] = String(text) + "\n\n[MUST REPLY IN ENGLISH ONLY]";

        String body;
        serializeJson(req_doc, body);

// ---- 发送 POST 请求 ----
        Serial.printf("[AICHAT] DeepSeek POST %u bytes...\n", body.length());
        unsigned long t0 = millis();
        int code = http.POST((uint8_t*)body.c_str(), body.length());
        unsigned long elapsed = millis() - t0;
        Serial.printf("[AICHAT] DeepSeek HTTP %d (%ums)\n", code, elapsed);

        if (code <= 0) {
            Serial.printf("[AICHAT] DeepSeek connection error: %d\n", code);
            http.end();
            client.stop();
            return String();
        }

// ---- 读取响应 body ----
// getString() 在新局部对象上可靠工作 — 无 end()/begin() 污染
        String resp;
        if (code == 200) {
            resp = http.getString();
            Serial.printf("[AICHAT] getString: %u bytes\n", resp.length());
        }
        http.end();
        client.stop();
        if (resp.length() == 0) {
            return String();
        }

        // ★ 精简打印
        Serial.printf("[AICHAT] Read body: %u bytes, first 200: %.200s\n", resp.length(), resp.c_str());

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, resp);
        if (err) { Serial.printf("[AICHAT] DeepSeek JSON parse fail: %s\n", err.c_str()); return String(); }

        const char* content = doc["choices"][0]["message"]["content"];
        if (!content) { Serial.println("[AICHAT] DeepSeek no content"); return String(); }

        String result(content);
        result.trim();
        Serial.printf("[DEEPSEEK] Parsed content (%u chars):\n%s\n--- end ---\n", result.length(), result.c_str());

// 过滤所有非 ASCII 字符
        String filtered;
        for (size_t i = 0; i < result.length(); i++) {
            char c = result[i];
            if (c >= 32 && c <= 126) filtered += c;
            else if (c == '\n' || c == '\r') filtered += c;
            else filtered += ' ';
        }
        filtered.trim();

// 迭代剥离包裹符号
        while (true) {
            int old_len = filtered.length();
            while ((filtered.startsWith("```") || filtered.startsWith("'''")) &&
                   (filtered.endsWith("```") || filtered.endsWith("'''"))) {
                filtered = filtered.substring(3, filtered.length() - 3);
                filtered.trim();
            }
            if (filtered.startsWith("`") && filtered.endsWith("`") && filtered.indexOf('\n') < 0) {
                filtered = filtered.substring(1, filtered.length() - 1);
                filtered.trim();
            }
            if (filtered.length() >= 2 && filtered[0] == '[' && filtered[filtered.length() - 1] == ']') {
                String inner = filtered.substring(1, filtered.length() - 1);
                inner.trim();
                if (inner.startsWith("```") || inner.startsWith("'''") ||
                    inner.startsWith("'") || inner.startsWith("`")) {
                    filtered = inner;
                }
            }
            while (filtered.length() >= 2) {
                char f = filtered[0];
                char e = filtered[filtered.length() - 1];
                if ((f == '\'' && e == '\'') || (f == '"' && e == '"')) {
                    filtered = filtered.substring(1, filtered.length() - 1);
                    filtered.trim();
                } else { break; }
            }
            if (filtered.length() == old_len) break;
        }

        Serial.printf("[AICHAT] DeepSeek reply OK, %u chars (filtered from %u)\n", filtered.length(), result.length());
        Serial.printf("[DEEPSEEK] Filtered text: \"%s\"\n", filtered.c_str());
        return filtered;
}

// ---- 此函数不再包含内层重试，外层 ai_chat_task 负责 retry ----


// ========== ai_chat_task — Core 0，持久任务（永不删除）==========
// Task 永活 (A1) 方案：
//   - 首次创建后永久存活，栈只分配一次，DRAM 碎片不累计
//   - 进出用信号量控制：s_aiChatSemaphore 唤醒 → 对话循环
//   - done 回到顶部等信号量，不清除句柄/PSRAM
//   - SSL/HTTP 全局单例，永不析构
//   - PSRAM 缓冲永久保留（ps_malloc 一次后永不 free）
static void ai_chat_task(void* pvParameters) {
    Serial.println("[AICHAT] persistent task started (A1)");
    s_aiChatTaskHandle = xTaskGetCurrentTaskHandle();

    String recognized, reply;
    int16_t* pcm_buffer = nullptr;
    size_t max_samples = SAMPLE_RATE * MAX_RECORD_SEC;
    size_t alloc_bytes = 0;
    size_t total_samples = 0;
    int16_t chunk_buf[CHUNK_SIZE];
    int high_cnt = 0;
    bool has_action = false;
    uint8_t saved_step = 16;

    // ★ 持久变量在所有轮次间保持
    bool first_run = true;
    bool psram_allocated = false;

    // ========== 主等待循环：等信号量 → 对话 → done → 回到等信号量 ==========
    while (true) {
        // ---- 等信号量（阻塞）----
        // 首次启动时 s_aiChatSemaphore 由 AI_Chat_Start 给信号
        // 后续轮次由 AI_Chat_Start() 给信号唤醒
        // AI_Chat_Stop() 也会给信号确保能从 wait 中退出
        // 超时 100ms 轮询一遍 ai_chat_active，防止信号量丢失造成永久阻塞
        while (s_aiChatSemaphore == NULL || xSemaphoreTake(s_aiChatSemaphore, pdMS_TO_TICKS(100)) != pdTRUE) {
            // 没有信号量对象 → 等创建
            if (s_aiChatSemaphore == NULL) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
            // 收到 Stop 信号但没信号量 → 重置状态
            if (!ai_chat_active) {
                // 没激活就继续等
            }
        }

        // 被唤醒后检查：如果是 Stop 信号，回到顶部继续等
        if (!ai_chat_active) {
            Serial.println("[AICHAT] woken by Stop, going idle");
            ai_chat_phase = AI_PHASE_IDLE;
            ai_chat_display_text[0] = '\0';
            ai_chat_dirty = true;
            continue;
        }

        Serial.println("[AICHAT] semaphore taken, starting conversation");

        // ---- 每轮开始：初始化/恢复环境 ----
        has_action = false;
        DRAW_SetStepSize(saved_step);

        // ★ 分配 PSRAM 缓冲（仅首次）
        if (!psram_allocated) {
            Serial.println("[AICHAT] alloc PSRAM buffer (permanent)...");
            alloc_bytes = max_samples * sizeof(int16_t);
            pcm_buffer = (int16_t*)ps_malloc(alloc_bytes);
            if (!pcm_buffer) {
                Serial.println("[AICHAT] PSRAM alloc FAILED!");
                ai_show(AI_PHASE_ERROR, "PSRAM alloc failed!");
                ai_chat_active = false;
                continue;
            }
            psram_allocated = true;
            Serial.printf("[AICHAT] PSRAM buffer: %u bytes (permanent)\n", alloc_bytes);
        }

        // ---- 初始化 MIC（每轮创建，对话结束后释放）----
        if (test_no_mic) {
            Serial.println("[AICHAT] TEST MODE: skipping MIC init");
        } else if (!s_mic) {
            Serial.println("[AICHAT] init MIC...");
            s_mic = new Microphone(MIC_SCK, MIC_WS, MIC_DATA, SAMPLE_RATE);
            if (!s_mic->init()) {
                Serial.println("[AICHAT] MIC init FAILED!");
                ai_show(AI_PHASE_ERROR, "MIC init FAILED!");
                delete s_mic; s_mic = nullptr;
                ai_chat_active = false;
                continue;
            }
            Serial.println("[AICHAT] MIC ready");
        }

        // ---- WiFi（首次连，后续 check alive）----
        if (first_run) {
            Serial.println("[AICHAT] connect WiFi...");
            if (!wifi_connect()) { Serial.println("[AICHAT] WiFi FAILED!"); ai_chat_active = false; continue; }
            Serial.println("[AICHAT] WiFi OK");
            first_run = false;
        } else {
            // ★ 每次会话前检查 WiFi 状态
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[AICHAT] WiFi lost, reconnecting...");
                WiFi.reconnect();
                int w = 0;
                while (WiFi.status() != WL_CONNECTED && w < 40) { delay(250); w++; }
                if (WiFi.status() != WL_CONNECTED) {
                    Serial.println("[AICHAT] WiFi reconnect failed");
                    ai_chat_active = false;
                    continue;
                }
            }
        }

        // ---- Baidu token（首次或过期重获）----
        if (strlen(baidu_token) == 0 || millis() / 1000 >= token_expires) {
            Serial.println("[AICHAT] get Baidu token...");
            if (!baidu_get_token()) {
                Serial.println("[AICHAT] Baidu token FAILED!");
                ai_show(AI_PHASE_ERROR, "Baidu token FAILED!");
                ai_chat_active = false;
                continue;
            }
            Serial.println("[AICHAT] Baidu token OK");
        }

        // ========== 内层对话循环（支持同一轮次内 re-record）==========
        while (ai_chat_active) {

        // wait_for_enter: 等待 ENTER 按钮（或测试信号）
        test_btn_triggered = false;  // ★ 清除上次 DONE 阶段可能的残留
        ai_show(AI_PHASE_WAITING, "");
        Serial.println("[AICHAT] WAITING for ENTER press...");
        Serial.flush();
        {
            bool pressed = false;
            while (!pressed && ai_chat_active) {
                // 测试模式：btn_triggered 或 btn_pressed
                if (test_btn_triggered) { test_btn_triggered = false; pressed = true; }
                else if (test_btn_pressed) { pressed = true; }
                else if (digitalRead(EN_S) == LOW) { pressed = true; }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (!ai_chat_active) { Serial.println("[AICHAT] cancelled while waiting ENTER"); goto conversation_done; }
        }
        vTaskDelay(pdMS_TO_TICKS(50));

    // ---- 确保 MIC 已初始化（轮次间可能已被释放，测试模式跳过）----
    if (!test_no_mic && !s_mic) {
        s_mic = new Microphone(MIC_SCK, MIC_WS, MIC_DATA, SAMPLE_RATE);
        if (!s_mic->init()) {
            Serial.println("[AICHAT] MIC re-init FAILED!");
            ai_show(AI_PHASE_ERROR, "MIC re-init FAILED!");
            delete s_mic; s_mic = nullptr;
            goto conversation_done;
        }
        Serial.println("[AICHAT] MIC re-initialized");
    }

    // ---- 录音到 PSRAM 缓冲区（测试模式：跳过录音，填充占位数据）----
    ai_show(AI_PHASE_RECORDING, "Recording...");
    total_samples = 0;
    high_cnt = 0;

    if (test_no_mic) {
        // 测试模式：填充 0.5s 纯静音 PCM，然后标记松手
        Serial.println("[AICHAT] TEST MODE: synthesizing dummy PCM...");
        size_t dummy_samples = SAMPLE_RATE / 2;
        for (size_t i = 0; i < dummy_samples && i < max_samples; i++) {
            pcm_buffer[i] = 0;
        }
        total_samples = dummy_samples;
        vTaskDelay(pdMS_TO_TICKS(100));
        Serial.printf("[AICHAT] TEST MODE: dummy PCM done (%zu samples)\n", total_samples);
    } else {
        while (total_samples < max_samples && ai_chat_active) {
            if (!ai_chat_active) { Serial.println("[AICHAT] cancelled during recording"); goto conversation_done; }
            if (digitalRead(EN_S) == HIGH) {
                high_cnt++;
                if (high_cnt > 15 && total_samples > SAMPLE_RATE / 4) break;
            } else {
                high_cnt = 0;
            }

            size_t n = s_mic->read(chunk_buf, CHUNK_SIZE);
            if (n == 0) break;
            memcpy(pcm_buffer + total_samples, chunk_buf, n * sizeof(int16_t));
            total_samples += n;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    Serial.printf("[AICHAT] recording done: %.1fs (%zu samples)\n", (float)total_samples / SAMPLE_RATE, total_samples);

    if (total_samples < SAMPLE_RATE / 4) {
        Serial.println("[AICHAT] recording too short!");
        ai_show(AI_PHASE_ERROR, "Recording too short!");
        vTaskDelay(pdMS_TO_TICKS(1500));
        if (s_mic) { delete s_mic; s_mic = nullptr; }
        continue;
    }

    // ---- 释放 MIC（测试模式无 MIC 可释放）----
    Serial.println("[AICHAT] release MIC");
    if (s_mic) { delete s_mic; s_mic = nullptr; }

    // ---- ASR（测试模式跳过，用固定文本）----
    if (test_no_mic) {
        recognized = "I suggest starting by showing them the basics of using the oscilloscope game console.";
        Serial.printf("[AICHAT] TEST MODE: ASR skipped, using canned text (%u chars)\n", recognized.length());
    } else {
        Serial.println("[AICHAT] start ASR...");
        ai_show(AI_PHASE_ASR, "");
        recognized = baidu_asr(pcm_buffer, total_samples);
    }
    if (recognized.length() == 0) {
        Serial.println("[AICHAT] ASR FAILED!");
        ai_show(AI_PHASE_ERROR, "ASR failed!");
        vTaskDelay(pdMS_TO_TICKS(1500));
        continue;
    }
    Serial.printf("[AICHAT] ASR OK: \"%s\"\n", recognized.c_str());

    // ---- DeepSeek ----
    Serial.println("[AICHAT] start DeepSeek...");
    ai_show(AI_PHASE_THINKING, "Contacting AI...");

    {
        const int max_attempts = 2;
        int attempt = 0;
        reply = String();
        while (attempt < max_attempts) {
            if (attempt > 0) {
                Serial.printf("[AICHAT] DeepSeek retry %d/%d\n", attempt + 1, max_attempts);
                Serial.printf("[AICHAT] heap before retry: %u\n", ESP.getFreeHeap());
                if (!wifi_ensure()) { reply = ""; break; }
                vTaskDelay(pdMS_TO_TICKS(1500));  // 给 SSL 内存回收足够时间
            }
            reply = deepseek_chat(recognized);
            if (reply.length() > 0) break;
            attempt++;
        }
    }

    if (reply.length() == 0) {
        Serial.println("[AICHAT] DeepSeek FAILED (empty reply)");
        Serial.printf("[AICHAT] heap after DeepSeek fail: %u\n", ESP.getFreeHeap());
        ai_show(AI_PHASE_ERROR, "AI response failed");
        voice_action = VC_NONE;
        has_action = false;
        vTaskDelay(pdMS_TO_TICKS(1500));
        goto conversation_done;
    }
    Serial.printf("[AICHAT] DeepSeek OK: %u chars\n", reply.length());

    // ---- 解析 JSON action ----
    Serial.println("[AICHAT] parse reply...");
    {
        String display = VC_ParseReply(reply);
        has_action = (voice_action != VC_NONE);
        Serial.printf("[AICHAT] VC_ParseReply returned display=\"%s\" (%u chars)\n", display.c_str(), display.length());
        Serial.printf("[AICHAT] has_action=%d voice_action=%d\n", has_action, (int)voice_action);

        ai_show(AI_PHASE_REPLY, display.c_str());
    }

    Serial.println("\n" + String(50, '-'));
    Serial.printf("AI: %s\n", ai_chat_display_text);
    Serial.println(String(50, '-') + "\n");

    // ---- 保持 REPLY 画面：guiTask 负责退出逻辑 ----
    if (has_action) {
        Serial.println("[AICHAT] has action, exiting for guiTask to execute");
        goto conversation_done;
    }

    // ---- pure text: poll ENTER button (short=exit, long=re-record) ----
    Serial.println("[AICHAT] pure text, phase=DONE, polling ENTER...");
    ai_show(AI_PHASE_DONE, NULL);

    {
        unsigned long press_start = 0;
        bool was_pressed = false;
        bool exit_task = false;

        while (!exit_task && ai_chat_active) {
            bool hw_pressed = (digitalRead(EN_S) == LOW);
            bool pressed = hw_pressed || test_btn_pressed;
            unsigned long now = millis();

            // 测试单次点击：直接标记为短按退出
            if (test_btn_triggered) {
                test_btn_triggered = false;
                Serial.println("[AICHAT] TEST: short click detected");
                exit_task = true;
                break;
            }

            if (pressed && !was_pressed) {
                press_start = now;
                was_pressed = true;
            } else if (pressed && was_pressed) {
                if (now - press_start >= 500) {
                    Serial.println("[AICHAT] Long press, continuing conversation...");
                    while (ai_chat_active && (digitalRead(EN_S) == LOW || test_btn_pressed)) {
                        vTaskDelay(pdMS_TO_TICKS(20));
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                    break;
                }
            } else if (!pressed && was_pressed) {
                was_pressed = false;
                if (now - press_start < 500) {
                    Serial.println("[AICHAT] Short press, exiting AI Chat...");
                    exit_task = true;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(20));
        }

        if (exit_task) break;
    }

    } // while(ai_chat_active) — 内层对话循环

conversation_done:
    // ★ A1 永活清理：释放临时资源，但保留 PSRAM/Task
    Serial.println("[AICHAT] conversation cleanup (A1 persistent task)");
    // ★ ESP-NOW 不在此恢复 — SSL 后 esp_now_init() 可能挂起
    // 由 Network_Manager::init() 在下次网络活动时重建
    ai_chat_dirty = true;
    recognized = String();
    reply = String();
    if (s_mic) { delete s_mic; s_mic = nullptr; }
    test_btn_triggered = false;  // ★ 清除跨轮残留
    // ★ PSRAM 缓冲永久保留 — 不 free(pcm_buffer)
    // ★ SSL/HTTP 局部对象已随 deepseek_chat 返回自动析构
    // ★ Task 不删除 — 回到顶部等信号量
    ai_chat_active = false;
    s_aiChatTaskHandle = xTaskGetCurrentTaskHandle();  // 句柄仍然有效
    Serial.printf("[AICHAT] heap at exit: %u\n", ESP.getFreeHeap());
    Serial.println("[AICHAT] conversation done, returning to idle (wait for semaphore)");

    // 回到 while(true) 顶部等信号量唤醒
    } // while(true) — 外层永活循环
}
