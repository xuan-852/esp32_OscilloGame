/**
 * @file ai_chat.cpp
 * @brief AI 瀵硅瘽 鈥?褰曢煶(INMP441)鈫掔櫨搴SR鈫扗eepSeek鈫掔ず娉㈠櫒鏄剧ず+鍔ㄤ綔璺宠浆
 *
 * 寮曡剼锛歁IC_SCK=47, MIC_WS=48, MIC_DATA=1 (瀹氫箟鍦?pins.h)
 *
 * 鍥涘ぇ璁捐鐩爣锛?
 *   1. 鑻辨枃鍥炲锛堢煝閲忓瓧绗﹀簱鍙湁 ASCII锛?
 *   2. 閫熺巼浼樺厛锛坉eepseek-v4-flash + 闈炴祦寮?+ HTTP/1.0锛?
 *   3. JSON 鍔ㄤ綔鎺у埗 AI 璺宠浆 SD 鍗?娓告垙
 *   4. 姣忛樁娈靛睆骞曟彁绀?
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
#include <lwip/sockets.h>      // TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT
#include "ai_config.h"


#define SAMPLE_RATE      16000
#define MAX_RECORD_SEC   10
#define CHUNK_SIZE       512

volatile bool ai_chat_active = false;

static Microphone* s_mic = nullptr;
static char baidu_token[256] = {0};
static unsigned long token_expires = 0;
static TaskHandle_t s_aiChatTaskHandle = NULL;
static WiFiClientSecure s_ds_client;           // 复用 SSL session（全局单例，永远不析构）
static HTTPClient s_http;                       // 复用 HTTPClient（全局单例，永远不析构）
static bool s_deepseek_inited = false;          // s_http 已 begin() + 设置好 headers

static bool     wifi_connect();
static bool     wifi_ensure();
static bool     baidu_get_token();
static String   baidu_asr(const int16_t* pcm, size_t samples);
static String   deepseek_chat(const String& text);
static void     ai_chat_task(void* pvParameters);

// ---- 闃舵鎻愮ず鏄剧ず ----
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
    // ★ SSL/HTTP 单例 — 永远不析构。退出重进完全无视连接状态。
    //   deepseek_chat() 内部自动处理 TCP 存活/断连重连。
    // 如果旧任务 handle 还非 NULL，说明上一轮未正常清理，强制删除
    if (s_aiChatTaskHandle != NULL) {
        Serial.println("[AICHAT] Force-killing stale task for re-entry");
        vTaskDelete(s_aiChatTaskHandle);
        s_aiChatTaskHandle = NULL;
    }

    // 清除可能残留的语音动作（防上一轮未消费）
    voice_pending = false;
    voice_action = VC_NONE;
    // 清除残留 MIC 指针
    if (s_mic) { delete s_mic; s_mic = nullptr; }
    // 防止 WiFi 省电中断 HTTPS（录音+ASR 耗时久，省电会断联）
    WiFi.setSleep(false);
    delay(500);  // 等待 WiFi 模组完全唤醒

    // ★ 检测 SSL/TCP 是否存活：WiFi 已连 + Token 缓存 + SSL 连接活着
    //   如果全部就绪，跳过 CONNECTING 画面，直接进 WAITING
    bool ssl_alive = s_deepseek_inited && s_ds_client.connected();
    bool wifi_ok   = (WiFi.status() == WL_CONNECTED);
    bool token_ok  = (strlen(baidu_token) > 0 && millis() / 1000 < token_expires);

    if (ssl_alive && wifi_ok && token_ok) {
        // SSL 保活 → 零开销复用，直接让用户按 ENTER 开始
        strncpy(ai_chat_display_text, phase_text(AI_PHASE_WAITING), sizeof(ai_chat_display_text) - 1);
        ai_chat_display_text[sizeof(ai_chat_display_text) - 1] = '\0';
        ai_chat_phase = AI_PHASE_WAITING;
        Serial.println("[AICHAT] SSL alive, skipping init display");
    } else {
        // 首次进入或连接已断 → 显示初始化状态
        strncpy(ai_chat_display_text, phase_text(AI_PHASE_CONNECTING), sizeof(ai_chat_display_text) - 1);
        ai_chat_display_text[sizeof(ai_chat_display_text) - 1] = '\0';
        ai_chat_phase = AI_PHASE_CONNECTING;
        Serial.println("[AICHAT] init display: CONNECTING");
    }
    ai_chat_dirty = true;

    ai_chat_active = true;
    if (xTaskCreatePinnedToCore(ai_chat_task, "AIChatTask", 16384, NULL, 1, &s_aiChatTaskHandle, 0) != pdPASS) {
        ai_chat_active = false;
        s_aiChatTaskHandle = NULL;
    }
}

void AI_Chat_Stop() {
    ai_chat_active = false;
    // 如果任务仍在运行，等待其自然退出（task 内检测 ai_chat_active 会 goto done）
}

// ---- 鍏变韩鍙橀噺 ----
char  ai_chat_display_text[2048];
volatile AIChatPhase ai_chat_phase = AI_PHASE_IDLE;
volatile bool ai_chat_dirty = false;

void ai_show(AIChatPhase phase, const char* text) {
    if (text && strlen(text) > 0) {
        strncpy(ai_chat_display_text, text, sizeof(ai_chat_display_text) - 1);
    } else if (text == NULL) {
        // text=NULL 鈫?鍙敼闃舵锛屼繚鐣欏凡鏈夋枃鏈?
        // 鐢ㄤ簬 DONE 闃舵涓嶈鐩栧洖澶嶅唴瀹?
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

// ---- 鐧惧害 Token ----
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

// ---- 鐧惧害 ASR ----
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

// ---- TCP 保活 — 防止 DeepSeek 服务器空闲超时断开 ----
// 在第一次成功 POST 后调用，lwIP 内核自动发空探测包维持 TCP
static void enable_tcp_keepalive() {
    int fd = s_ds_client.fd();
    if (fd < 0) { Serial.println("[DSEEK] keepalive: no fd yet"); return; }
    int keepalive = 1;
    int keepidle  = 30;   // 空闲 30s 后开始探测
    int keepintvl = 10;   // 每次探测间隔 10s
    int keepcnt   = 4;    // 连续 4 次无响应 → 判定断开（总计 30+40=70s）
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &keepidle,  sizeof(keepidle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &keepcnt,   sizeof(keepcnt));
    Serial.println("[DSEEK] TCP keepalive enabled (30s idle, 10s intvl, 4 probes)");
}

// ---- DeepSeek 对话（HTTPClient 静态单例 + TCP 保活）
// s_ds_client + s_http 都是全局静态对象，永远不析构 → SSL/TLS 连接永不意外被释放
// 核心策略：
//   1. TCP 连接幸存 → HTTPClient::connect() 检测 connected()=true，零开销复用
//   2. TCP 超时断开 → stop_ssl_socket() 完整释放 mbedTLS + memset
//                      delay(100) 让堆合并 → 重连（DRAM 大块从零分配，无碎片累积）
//   3. 退出/重进 → 不碰 SSL，存活连接保持，断连走#2
static String deepseek_chat(const String& text) {
    Serial.printf("[DSEEK] heap: %u  WiFi RSSI: %d  SSL.connected: %d\n",
                  ESP.getFreeHeap(), WiFi.RSSI(), s_ds_client.connected());

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

    // ---- HTTPClient 单例初始化（只做一次）----
    // 策略：HTTPClient 和 WiFiClientSecure 都是 static 全局，永不析构。响应后用 replace=true
    // 替换 header 避免累积。getString() 内部 disconnect(true) 在 keep-alive 时不杀 TCP/SSL。
    // SSL 断连时 → end()+stop()+begin() 完整重建。
    if (!s_deepseek_inited) {
        s_ds_client.setInsecure();
        s_http.setReuse(true);       // HTTP/1.1 keep-alive
        s_http.setTimeout(30000);    // 响应读超时 30s
        s_http.addHeader("Content-Type", "application/json");
        s_http.begin(s_ds_client, "https://api.deepseek.com/chat/completions");
        s_deepseek_inited = true;
        Serial.println("[DSEEK] HTTPClient initialized");
    }

    String result;
    for (int attempt = 0; attempt < 2 && result.length() == 0; attempt++) {
        if (attempt > 0) {
            Serial.printf("[DSEEK] retry %d\n", attempt + 1);
            // 重试：完整断开清理
            s_http.end();             // disconnect(false) + clear()
            s_ds_client.stop();       // stop_ssl_socket → mbedTLS 释放 + socket 关闭 + memset(0)
            delay(1500);              // 堆碎片合并
            s_http.addHeader("Content-Type", "application/json");
            s_http.begin(s_ds_client, "https://api.deepseek.com/chat/completions");
        }

        // ---- TCP/SSL 断连时完整清理重连 ----
        if (!s_ds_client.connected()) {
            Serial.println("[DSEEK] SSL not connected, reconnecting...");
            s_http.end();             // clear() → 擦 _headers
            s_ds_client.stop();       // stop_ssl_socket → 完整 mbedTLS 释放 + memset(0)
            delay(100);               // 堆碎片合并
            s_ds_client.setInsecure();// 标志位
            s_http.addHeader("Content-Type", "application/json");
            s_http.begin(s_ds_client, "https://api.deepseek.com/chat/completions");
            Serial.println("[DSEEK] SSL re-init done");
        }

        // ---- 构造 JSON body ----
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

        // ---- 发 POST（HTTPClient，不析构）----
        // replace=true → 替换旧值而非累积，_headers 永不膨胀
        s_http.addHeader("Authorization", "Bearer " + String(DEEPSEEK_API_KEY), false, true);
        Serial.printf("[AICHAT] DeepSeek POST %u bytes...\n", body.length());
        unsigned long t0 = millis();
        int code = s_http.POST((uint8_t*)body.c_str(), body.length());
        Serial.printf("[AICHAT] DeepSeek HTTP %d (%ums)\n", code, millis() - t0);

        if (code <= 0) {
            // 网络错误：连接已废，清理后重试
            Serial.printf("[AICHAT] DeepSeek connection error: %d\n", code);
            s_http.end();
            s_ds_client.stop();
            continue;
        }

        // ---- 读响应 body（getString 内部 disconnect(true)，keep-alive 不杀 SSL）----
        String resp = s_http.getString();

        if (code != 200) {
            Serial.printf("[AICHAT] DeepSeek HTTP %d, resp: %s\n", code, resp.substring(0, 200).c_str());
            s_http.end();
            s_ds_client.stop();
            continue;
        }
        if (resp.length() == 0) { Serial.println("[AICHAT] DeepSeek empty response"); continue; }

        // ★ 不调 end() — getString 内部已调 disconnect(true)，keep-alive 时 TCP/SSL 保持，
        //   Connection:close 时 _client 指针保留，下次 connect() 自动重连 SSL。headers 用
        //   replace=true 替换不累积。

        // ---- 每次成功 HTTP 200 后保证 TCP 保活开启（幂等，重连后新 fd 同样生效）----
        enable_tcp_keepalive();

        // 打印原始响应
        Serial.println("===== DEEPSEEK RAW RESPONSE (first 500 chars) =====");
        Serial.println(resp.substring(0, 500));
        Serial.println("===== END RAW RESPONSE =====");

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, resp);
        if (err) { Serial.printf("[AICHAT] DeepSeek JSON parse fail: %s\n", err.c_str()); continue; }

        const char* content = doc["choices"][0]["message"]["content"];
        if (!content) { Serial.println("[AICHAT] DeepSeek no content"); continue; }

        result = String(content);
        result.trim();
        Serial.printf("[DEEPSEEK] Parsed content (%u chars):\n%s\n--- end ---\n", result.length(), result.c_str());

        // 过滤所有非 ASCII 字符（矢量字库只支持 ASCII）
        String filtered;
        for (size_t i = 0; i < result.length(); i++) {
            char c = result[i];
            if (c >= 32 && c <= 126) filtered += c;
            else if (c == '\n' || c == '\r') filtered += c;
            else filtered += ' ';
        }
        filtered.trim();

        // 迭代剥离各种包裹符号，直到无法再剥离
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

    // 所有重试都失败
    Serial.printf("[DSEEK] all attempts failed, heap: %u\n", ESP.getFreeHeap());
    return String();
}


//  ai_chat_task — Core 0, 完整流程（内部对话循环，SSL 连接永不释放）
// ============================================================
static void ai_chat_task(void* pvParameters) {
    Serial.println("[AICHAT] task started");
    ai_chat_active = true;

    String recognized, reply;
    int16_t* pcm_buffer = nullptr;
    size_t total_samples = 0;
    size_t max_samples = SAMPLE_RATE * MAX_RECORD_SEC;
    size_t alloc_bytes = 0;
    int16_t chunk_buf[CHUNK_SIZE];
    int high_cnt = 0;
    bool has_action = false;
    uint8_t saved_step = 16;  // 绱у噾妯″紡

    // AI_Chat_Start() 已设 CONNECTING，初始化完成后对话循环内才切到 WAITING
    Serial.printf("[AICHAT] heap free: %u\n", ESP.getFreeHeap());

    // ---- 设置 step（矢量子系统参数） ----
    DRAW_SetStepSize(saved_step);

    // ---- Init MIC ----
    Serial.println("[AICHAT] init MIC...");
    if (!s_mic) {
        s_mic = new Microphone(MIC_SCK, MIC_WS, MIC_DATA, SAMPLE_RATE);
        if (!s_mic->init()) {
            Serial.println("[AICHAT] MIC init FAILED!");
            ai_show(AI_PHASE_ERROR, "MIC init FAILED!");
            delete s_mic; s_mic = nullptr;
            goto done;
        }
        Serial.println("[AICHAT] MIC ready");
    }

    // ---- WiFi ----
    Serial.println("[AICHAT] connect WiFi...");
    if (!wifi_connect()) { Serial.println("[AICHAT] WiFi FAILED!"); goto done; }
    Serial.println("[AICHAT] WiFi OK");

    // ---- Baidu token ----
    Serial.println("[AICHAT] get Baidu token...");
    if (!baidu_get_token()) {
        Serial.println("[AICHAT] Baidu token FAILED!");
        ai_show(AI_PHASE_ERROR, "Baidu token FAILED!");
        goto done;
    }
    Serial.println("[AICHAT] Baidu token OK");

    // ---- 鍒嗛厤 PSRAM ----
    Serial.println("[AICHAT] alloc PSRAM buffer...");
    alloc_bytes = max_samples * sizeof(int16_t);
    pcm_buffer = (int16_t*)ps_malloc(alloc_bytes);
    if (!pcm_buffer) {
        Serial.println("[AICHAT] PSRAM alloc FAILED!");
        ai_show(AI_PHASE_ERROR, "PSRAM alloc failed!");
        goto done;
    }
    Serial.printf("[AICHAT] PSRAM buffer: %u bytes\n", alloc_bytes);

    // ========== 对话轮次循环（SSL/HTTP/WiFi 连接永不释放）==========
    Serial.println("[AICHAT] entering conversation loop");
    while (ai_chat_active) {

        ai_show(AI_PHASE_WAITING, "");
        Serial.println("[AICHAT] WAITING for ENTER press...");
        while (digitalRead(EN_S) == HIGH) {
            if (!ai_chat_active) { Serial.println("[AICHAT] cancelled while waiting ENTER"); goto done; }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vTaskDelay(pdMS_TO_TICKS(50));

    // ---- 确保 MIC 已初始化（轮次间可能已释放）----
    if (!s_mic) {
        s_mic = new Microphone(MIC_SCK, MIC_WS, MIC_DATA, SAMPLE_RATE);
        if (!s_mic->init()) {
            Serial.println("[AICHAT] MIC re-init FAILED!");
            ai_show(AI_PHASE_ERROR, "MIC re-init FAILED!");
            delete s_mic; s_mic = nullptr;
            goto done;
        }
        Serial.println("[AICHAT] MIC re-initialized");
    }

    // ---- 录音到 PSRAM ----
    ai_show(AI_PHASE_RECORDING, "Recording...");
    total_samples = 0;
    high_cnt = 0;

    while (total_samples < max_samples) {
        if (!ai_chat_active) { Serial.println("[AICHAT] cancelled during recording"); goto done; }
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

    Serial.printf("[AICHAT] recording done: %.1fs (%zu samples)\n", (float)total_samples / SAMPLE_RATE, total_samples);

    if (total_samples < SAMPLE_RATE / 4) {
        Serial.println("[AICHAT] recording too short!");
        ai_show(AI_PHASE_ERROR, "Recording too short!");
        vTaskDelay(pdMS_TO_TICKS(1500));
        if (s_mic) { delete s_mic; s_mic = nullptr; }
        continue;
    }

    // ---- 閲婃斁 MIC I2S ----
    Serial.println("[AICHAT] release MIC");
    if (s_mic) { delete s_mic; s_mic = nullptr; }

    // ---- ASR ----
    Serial.println("[AICHAT] start ASR...");
    ai_show(AI_PHASE_ASR, "");
    recognized = baidu_asr(pcm_buffer, total_samples);
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
        continue;
    }
    Serial.printf("[AICHAT] DeepSeek OK: %u chars\n", reply.length());

    // ---- 瑙ｆ瀽 JSON action ----
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

    // ---- 淇濇寔 REPLY 鐢婚潰锛実uiTask 璐熻矗閫€鍑?----
    // 鏈夊姩浣滐細灞曠ず鍥炲锛実uiTask 妫€娴?voice_pending 鍚庝細鎵ц璺宠浆
    // 绾枃鏈細鐩存帴缁撴潫浠诲姟锛岃 guiTask 鎺ョ閫€鍑洪€昏緫
    if (has_action) {
        Serial.println("[AICHAT] has action, exiting for guiTask to execute");
        goto done;
    }

    // ---- pure text: poll ENTER button (short=exit, long=re-record) ----
    Serial.println("[AICHAT] pure text, phase=DONE, polling ENTER...");
    ai_show(AI_PHASE_DONE, NULL);

    {
        unsigned long press_start = 0;
        bool was_pressed = false;
        bool exit_task = false;

        while (!exit_task && ai_chat_active) {
            bool pressed = (digitalRead(EN_S) == LOW);
            unsigned long now = millis();

            if (pressed && !was_pressed) {
                press_start = now;
                was_pressed = true;
            } else if (pressed && was_pressed) {
                if (now - press_start >= 500) {
                    Serial.println("[AICHAT] Long press, continuing conversation...");
                    while (ai_chat_active && digitalRead(EN_S) == LOW) {
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

    } // while(ai_chat_active)

done:
    Serial.println("[AICHAT] cleanup...");
    ai_chat_dirty = true;
    // ★ 显式释放栈上 String（vTaskDelete 不调 C++ 析构函数，内部堆缓冲会泄漏）
    recognized = String();   // 释放 baidu_asr 结果的内部缓冲
    reply = String();        // 释放 deepseek_chat 结果的内部缓冲
    // ★ SSL/HTTP 单例默认保持 — s_ds_client + s_http 是全局静态，不析构。
    //   如果 TCP 活着(保活开启) → 下次 deepseek_chat() 零开销复用。
    //   如果 TCP 断了 → deepseek_chat() 内自动 stop + delay + 重连。
    if (s_mic) { delete s_mic; s_mic = nullptr; }
    if (pcm_buffer) { free(pcm_buffer); Serial.println("[AICHAT] pcm_buffer freed"); }
    // 注意：故意不恢复 WiFi 省电模式
    // 如果这里调用 WiFi.setSleep(true)，下次 AI Chat 时 WiFi 模组仍未完全就绪，
    // 会导致第二次 DeepSeek HTTPS/SSL 握手持续失败
    ai_chat_active = false;
    s_aiChatTaskHandle = NULL;
    Serial.printf("[AICHAT] heap at exit: %u\n", ESP.getFreeHeap());
    Serial.println("[AICHAT] task exits, vTaskDelete");
    vTaskDelete(NULL);
}
