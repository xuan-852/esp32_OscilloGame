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
#include "ai_config.h"


#define SAMPLE_RATE      16000
#define MAX_RECORD_SEC   10
#define CHUNK_SIZE       512

volatile bool ai_chat_active = false;

static Microphone* s_mic = nullptr;
static char baidu_token[256] = {0};
static unsigned long token_expires = 0;

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
    if (ai_chat_active) return;
    ai_chat_active = true;
    if (xTaskCreatePinnedToCore(ai_chat_task, "AIChatTask", 16384, NULL, 1, NULL, 0) != pdPASS) {
        ai_chat_active = false;
    }
}

void AI_Chat_Stop() {
    ai_chat_active = false;
}

// ---- 鍏变韩鍙橀噺 ----
char  ai_chat_display_text[512];
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
    if (code != 200) { http.end(); return String(); }

    String resp = http.getString();
    http.end();
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

// ---- DeepSeek 对话（HTTPClient，PSRAM 已启用所以 SSL 不报 -76）
static String deepseek_chat(const String& text) {
    // === 诊断：DNS 解析 ===
    Serial.println("[DSEEK] Resolving api.deepseek.com...");
    IPAddress ip;
    if (!WiFi.hostByName("api.deepseek.com", ip)) {
        Serial.println("[DSEEK] DNS RESOLVE FAILED!");
    } else {
        Serial.printf("[DSEEK] DNS OK: %s\n", ip.toString().c_str());
    }

    // === 诊断：TCP 连通性测试 ===
    Serial.println("[DSEEK] Testing TCP to api.deepseek.com:443...");
    WiFiClient tcp_test;
    unsigned long t0 = millis();
    bool tcp_ok = tcp_test.connect(ip, 443, 5000);
    if (tcp_ok) {
        Serial.printf("[DSEEK] TCP OK (%ums)\n", millis() - t0);
        tcp_test.stop();
    } else {
        Serial.printf("[DSEEK] TCP FAILED (%ums)\n", millis() - t0);
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.begin(client, "https://api.deepseek.com/chat/completions");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + DEEPSEEK_API_KEY);
    http.useHTTP10(true);
    http.setTimeout(30000);
    http.setConnectTimeout(10000);

    JsonDocument req_doc;
    req_doc["model"] = "deepseek-v4-flash";
    req_doc["temperature"] = 0.7;
    req_doc["max_tokens"] = 200;
    req_doc["stream"] = false;
    req_doc["thinking"]["type"] = "disabled";

    JsonArray messages = req_doc["messages"].to<JsonArray>();
    JsonObject sys = messages.add<JsonObject>();
    sys["role"] = "system";
    sys["content"] =
        "You control an oscilloscope game console. Reply STRICTLY in ENGLISH ONLY. "
        "ABSOLUTELY NEVER use Chinese or any non-ASCII characters. "
        "Keep replies under 80 words, 5 lines max. "
        "Pure ASCII text only, no accented letters, no Unicode.\n"
        "For navigation actions, reply with JSON (no markdown):\n"
        "{\"reply\":\"...\",\"action\":\"action_name\"}\n"
        "Actions: open_music, open_video, open_games, open_online, "
        "open_game_joy, open_ai_chat, open_about, "
        "start_snake, start_breakout, start_flappy, start_racing, "
        "start_runtiny, start_tank, back, exit";

    JsonObject user = messages.add<JsonObject>();
    user["role"] = "user";
    user["content"] = text;

    String body;
    serializeJson(req_doc, body);

    Serial.printf("[AICHAT] DeepSeek POST %u bytes...\n", body.length());

    int code = http.POST(body);
    Serial.printf("[AICHAT] DeepSeek HTTP %d\n", code);

    String resp = http.getString();
    http.end();

    if (code != 200) {
        Serial.printf("[AICHAT] DeepSeek HTTP %d, resp: %s\n", code, resp.substring(0, 300).c_str());
        return String();
    }

    if (resp.length() == 0) {
        Serial.println("[AICHAT] DeepSeek empty response");
        return String();
    }

    // ★ 打印原始 HTTP 响应前 500 字符
    Serial.println("===== DEEPSEEK RAW RESPONSE (first 500 chars) =====");
    Serial.println(resp.substring(0, 500));
    Serial.println("===== END RAW RESPONSE =====");

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, resp);
    if (err) {
        Serial.printf("[AICHAT] DeepSeek JSON parse fail: %s\n", err.c_str());
        return String();
    }

    const char* content = doc["choices"][0]["message"]["content"];
    if (!content) {
        Serial.println("[AICHAT] DeepSeek no content in response");
        return String();
    }

    String result(content);
    result.trim();

    // ★ 打印解析后的 content
    Serial.printf("[DEEPSEEK] Parsed content (%u chars):\n", result.length());
    Serial.println(result);
    Serial.println("[DEEPSEEK] --- end content ---");

    // 过滤所有非 ASCII 字符（矢量字库只支持 ASCII）
    String filtered;
    for (size_t i = 0; i < result.length(); i++) {
        char c = result[i];
        if (c >= 32 && c <= 126) filtered += c;
        else if (c == '\n' || c == '\r') filtered += c;
        else filtered += ' ';  // 替代不可见字符
    }
    filtered.trim();

    Serial.printf("[AICHAT] DeepSeek reply OK, %u chars (filtered from %u)\n", filtered.length(), result.length());
    // ★ 打印过滤后的文本
    Serial.printf("[DEEPSEEK] Filtered text: \"%s\"\n", filtered.c_str());
    return filtered;
}


//  ai_chat_task — Core 0, 完整流程
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

    ai_chat_phase = AI_PHASE_IDLE;
    ai_chat_dirty = true;

    Serial.printf("[AICHAT] heap free: %u\n", ESP.getFreeHeap());

    // ---- 鏄剧ず "Press ENTER to record" ----
    DRAW_SetStepSize(saved_step);
    ai_show(AI_PHASE_WAITING, "");
    Serial.println("[AICHAT] phase=WAITING");

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

    // ---- 绛夊緟 ENTER 鎸変笅褰曢煶 ----
    ai_show(AI_PHASE_WAITING, "");
    Serial.println("[AICHAT] WAITING for ENTER press...");
    while (digitalRead(EN_S) == HIGH) {
        if (!ai_chat_active) { Serial.println("[AICHAT] cancelled while waiting ENTER"); goto done; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    Serial.println("[AICHAT] ENTER pressed, debounce...");
    // 绛夊緟閲婃斁寮€濮嬪綍闊筹紙闃叉姈鍔級
    vTaskDelay(pdMS_TO_TICKS(50));
    Serial.println("[AICHAT] start recording");

    // ---- 褰曢煶鍒?PSRAM ----
    ai_show(AI_PHASE_RECORDING, "");
    Serial.println("[AICHAT] Recording... release to stop");

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
        goto done;
    }

    // ---- 閲婃斁 MIC I2S ----
    Serial.println("[AICHAT] release MIC");
    if (s_mic) { delete s_mic; s_mic = nullptr; }

    // ---- ASR ----
    Serial.println("[AICHAT] start ASR...");
    recognized = baidu_asr(pcm_buffer, total_samples);
    if (recognized.length() == 0) {
        Serial.println("[AICHAT] ASR FAILED!");
        ai_show(AI_PHASE_ERROR, "ASR failed!");
        goto done;
    }
    Serial.printf("[AICHAT] ASR OK: \"%s\"\n", recognized.c_str());

    // ---- DeepSeek ----
    Serial.println("[AICHAT] start DeepSeek...");
    ai_show(AI_PHASE_THINKING, "Contacting AI...");

    {
        const int max_attempts = 2; // flash 妯″瀷蹇紝鍑忓皯閲嶈瘯娆℃暟
        int attempt = 0;
        while (attempt < max_attempts) {
            if (attempt > 0) {
                Serial.printf("[AICHAT] DeepSeek retry %d/%d\n", attempt + 1, max_attempts);
                if (!wifi_ensure()) { reply = ""; break; }
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            reply = deepseek_chat(recognized);
            if (reply.length() > 0) break;
            attempt++;
        }
    }

    if (reply.length() == 0) {
        Serial.println("[AICHAT] DeepSeek FAILED (empty reply)");
        ai_show(AI_PHASE_ERROR, "AI response failed");
        voice_action = VC_NONE;
        has_action = false;
        vTaskDelay(pdMS_TO_TICKS(1500));
        goto done;
    }
    Serial.printf("[AICHAT] DeepSeek OK: %u chars\n", reply.length());

    // ---- 瑙ｆ瀽 JSON action ----
    Serial.println("[AICHAT] parse reply...");
    {
        String display = VC_ParseReply(reply);
        has_action = (voice_action != VC_NONE);
        Serial.printf("[AICHAT] VC_ParseReply returned display=\"%s\" (%u chars)\n", display.c_str(), display.length());
        Serial.printf("[AICHAT] has_action=%d voice_action=%d\n", has_action, (int)voice_action);

        if (has_action && display.length() > 0) {
            ai_show(AI_PHASE_REPLY, display.c_str());
        } else {
            ai_show(AI_PHASE_REPLY, reply.c_str());
        }
    }

    Serial.println("\n" + String(50, '-'));
    Serial.printf("AI: %s\n", ai_chat_display_text);
    Serial.println(String(50, '-') + "\n");

    // ---- 淇濇寔 REPLY 鐢婚潰锛実uiTask 璐熻矗閫€鍑?----
    // 鏈夊姩浣滐細灞曠ず鍥炲锛実uiTask 妫€娴?voice_pending 鍚庝細鎵ц璺宠浆
    // 绾枃鏈細鐩存帴缁撴潫浠诲姟锛岃 guiTask 鎺ョ閫€鍑洪€昏緫
    if (has_action) {
        Serial.println("[AICHAT] has action, exiting for guiTask to execute");
        goto done; // voice_pending 鐢?guiTask 娑堣垂
    }

    // 绾枃鏈洖澶?鈥?淇濇寔 REPLY 鐢婚潰涓嶈鐩栵紝璁╃敤鎴风湅瀹?
    // guiTask 浼氬湪 AI_PHASE_REPLY/DONE 闃舵鍝嶅簲鎸夐挳閫€鍑?
    // 鐢?NULL 淇濇寔宸叉湁鏂囨湰涓嶅彉
    Serial.println("[AICHAT] pure text, phase=DONE, wait for ENTER");
    ai_show(AI_PHASE_DONE, NULL);

done:
    Serial.println("[AICHAT] cleanup...");
    ai_chat_dirty = true;
    if (s_mic) { delete s_mic; s_mic = nullptr; }
    if (pcm_buffer) { free(pcm_buffer); Serial.println("[AICHAT] pcm_buffer freed"); }
    ai_chat_active = false;
    Serial.println("[AICHAT] task exits, vTaskDelete");
    vTaskDelete(NULL);
}
