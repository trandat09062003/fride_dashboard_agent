#include "groq_client.h"
#include "config_ai.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

namespace GroqAI {

static const char *GROQ_HOST = "api.groq.com";
static const int GROQ_PORT = 443;

static WiFiClientSecure s_tls_client;
static SemaphoreHandle_t s_tls_mutex = nullptr;
static bool s_tls_initialized = false;

static void ensure_mutex() {
    if (!s_tls_mutex) {
        s_tls_mutex = xSemaphoreCreateMutex();
    }
    if (!s_tls_initialized) {
        s_tls_client.setInsecure();
        s_tls_client.setTimeout(8);
        s_tls_initialized = true;
    }
}

void init() {
    ensure_mutex();
    Serial.println("[GROQ] Groq AI Client Initialized (Keep-Alive + TLS Prewarm enabled).");
}

static bool ensure_connected_locked() {
    if (s_tls_client.connected()) {
        // Flush any stale unread bytes from previous keep-alive response
        while (s_tls_client.available()) {
            s_tls_client.read();
        }
        return true;
    }
    s_tls_client.stop();
    s_tls_client.setInsecure();
    s_tls_client.setTimeout(8);
    unsigned long t0 = millis();
    bool ok = s_tls_client.connect(GROQ_HOST, GROQ_PORT);
    if (ok) {
        Serial.printf("[GROQ] TLS connected in %lu ms\n", millis() - t0);
    }
    return ok;
}

static void prewarm_worker_task(void *param) {
    if (WiFi.status() == WL_CONNECTED) {
        ensure_mutex();
        if (xSemaphoreTake(s_tls_mutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
            ensure_connected_locked();
            xSemaphoreGive(s_tls_mutex);
        }
    }
    vTaskDelete(NULL);
}

void prewarm_connection_async() {
    if (WiFi.status() != WL_CONNECTED) return;
    ensure_mutex();
    xTaskCreatePinnedToCore(
        prewarm_worker_task,
        "groq_warm",
        8192,
        NULL,
        1,
        NULL,
        0 // Run TLS handshake on Core 0 while Core 1 records audio!
    );
}

// Fast HTTP response reader supporting Content-Length, Chunked encoding & JSON brace completion
static bool read_http_json_response(WiFiClientSecure &client, String &out_body, uint32_t timeout_ms) {
    unsigned long start_ms = millis();
    while (client.connected() && client.available() == 0) {
        if (millis() - start_ms > timeout_ms) {
            Serial.println("[GROQ] HTTP wait timeout!");
            client.stop();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    int content_length = -1;
    bool is_chunked = false;
    bool conn_close = false;
    int status_code = 0;

    // 1. Parse HTTP status & headers
    String status_line = client.readStringUntil('\n');
    status_line.trim();
    if (status_line.startsWith("HTTP/1.")) {
        int sp1 = status_line.indexOf(' ');
        if (sp1 > 0) {
            status_code = status_line.substring(sp1 + 1).toInt();
        }
    }

    while (client.connected() || client.available()) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) break;

        String lower = line;
        lower.toLowerCase();
        if (lower.startsWith("content-length:")) {
            content_length = lower.substring(15).toInt();
        } else if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") >= 0) {
            is_chunked = true;
        } else if (lower.startsWith("connection:") && lower.indexOf("close") >= 0) {
            conn_close = true;
        }
    }

    out_body = "";
    if (content_length > 0) {
        out_body.reserve(content_length + 16);
        int remaining = content_length;
        unsigned long read_t = millis();
        uint8_t buf[512];
        while (remaining > 0 && (client.connected() || client.available()) && (millis() - read_t < 5000)) {
            int avail = client.available();
            if (avail > 0) {
                int to_read = (avail < remaining) ? avail : remaining;
                if (to_read > (int)sizeof(buf)) to_read = sizeof(buf);
                int r = client.read(buf, to_read);
                if (r > 0) {
                    out_body.concat((const char *)buf, r);
                    remaining -= r;
                    read_t = millis();
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }
    } else if (is_chunked) {
        unsigned long read_t = millis();
        while ((client.connected() || client.available()) && (millis() - read_t < 5000)) {
            String hex_len = client.readStringUntil('\n');
            hex_len.trim();
            if (hex_len.length() == 0) continue;
            long chunk_sz = strtol(hex_len.c_str(), NULL, 16);
            if (chunk_sz <= 0) {
                // Consume trailing \r\n after 0 chunk
                client.readStringUntil('\n');
                break;
            }
            int rem = (int)chunk_sz;
            uint8_t buf[512];
            while (rem > 0 && (client.connected() || client.available()) && (millis() - read_t < 5000)) {
                int avail = client.available();
                if (avail > 0) {
                    int to_read = (avail < rem) ? avail : rem;
                    if (to_read > (int)sizeof(buf)) to_read = sizeof(buf);
                    int r = client.read(buf, to_read);
                    if (r > 0) {
                        out_body.concat((const char *)buf, r);
                        rem -= r;
                        read_t = millis();
                    }
                } else {
                    vTaskDelay(pdMS_TO_TICKS(2));
                }
            }
            // Consume chunk trailing \r\n
            client.readStringUntil('\n');
        }
    } else {
        // Fallback: read until JSON root object closes or connection closes
        int brace_depth = 0;
        bool in_str = false;
        bool esc = false;
        bool started_json = false;
        unsigned long read_t = millis();
        while ((client.connected() || client.available()) && (millis() - read_t < 4000)) {
            while (client.available() > 0) {
                char c = (char)client.read();
                out_body += c;
                read_t = millis();
                if (esc) { esc = false; continue; }
                if (c == '\\' && in_str) { esc = true; continue; }
                if (c == '"') { in_str = !in_str; continue; }
                if (!in_str) {
                    if (c == '{') { brace_depth++; started_json = true; }
                    else if (c == '}') {
                        brace_depth--;
                        if (started_json && brace_depth <= 0) break;
                    }
                }
            }
            if (started_json && brace_depth <= 0) break;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    if (conn_close) {
        client.stop();
    }

    return (status_code == 200 && out_body.length() > 0);
}

// Helper to construct a standard 44-byte WAV header
static void build_wav_header(uint8_t *header, uint32_t pcm_data_len, uint32_t sample_rate, uint16_t channels, uint16_t bits_per_sample) {
    uint32_t total_data_len = pcm_data_len + 36;
    uint32_t byte_rate = sample_rate * channels * bits_per_sample / 8;
    uint16_t block_align = channels * bits_per_sample / 8;

    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    header[4] = (uint8_t)(total_data_len & 0xff);
    header[5] = (uint8_t)((total_data_len >> 8) & 0xff);
    header[6] = (uint8_t)((total_data_len >> 16) & 0xff);
    header[7] = (uint8_t)((total_data_len >> 24) & 0xff);
    header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';

    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
    header[20] = 1;  header[21] = 0;
    header[22] = (uint8_t)(channels & 0xff); header[23] = (uint8_t)((channels >> 8) & 0xff);
    header[24] = (uint8_t)(sample_rate & 0xff);
    header[25] = (uint8_t)((sample_rate >> 8) & 0xff);
    header[26] = (uint8_t)((sample_rate >> 16) & 0xff);
    header[27] = (uint8_t)((sample_rate >> 24) & 0xff);
    header[28] = (uint8_t)(byte_rate & 0xff);
    header[29] = (uint8_t)((byte_rate >> 8) & 0xff);
    header[30] = (uint8_t)((byte_rate >> 16) & 0xff);
    header[31] = (uint8_t)((byte_rate >> 24) & 0xff);
    header[32] = (uint8_t)(block_align & 0xff);
    header[33] = (uint8_t)((block_align >> 8) & 0xff);
    header[34] = (uint8_t)(bits_per_sample & 0xff);
    header[35] = (uint8_t)((bits_per_sample >> 8) & 0xff);

    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    header[40] = (uint8_t)(pcm_data_len & 0xff);
    header[41] = (uint8_t)((pcm_data_len >> 8) & 0xff);
    header[42] = (uint8_t)((pcm_data_len >> 16) & 0xff);
    header[43] = (uint8_t)((pcm_data_len >> 24) & 0xff);
}

// 1. Speech-To-Text via Groq Whisper Turbo API (Reuses Pre-Warmed Keep-Alive TLS Socket)
bool speech_to_text(const int16_t *samples, size_t sample_count, char *out_text, size_t max_out_len) {
    if (!samples || sample_count == 0 || !out_text) return false;
    if (strlen(GROQ_API_KEY) == 0) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    ensure_mutex();
    if (xSemaphoreTake(s_tls_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return false;

    unsigned long t_start = millis();
    Serial.printf("[GROQ] Sending %d audio samples (%d bytes) to Whisper Turbo STT...\n",
                  (int)sample_count, (int)(sample_count * 2));

    const char *boundary = "----GroqBoundary12345";
#ifdef GROQ_STT_MODEL
    const char *stt_model = GROQ_STT_MODEL;
#else
    const char *stt_model = "whisper-large-v3-turbo";
#endif

    String part_hdrs = String("--") + boundary + "\r\n" +
                       "Content-Disposition: form-data; name=\"model\"\r\n\r\n" +
                       stt_model + "\r\n" +
                       "--" + boundary + "\r\n" +
                       "Content-Disposition: form-data; name=\"language\"\r\n\r\n" +
                       "vi\r\n" +
                       "--" + boundary + "\r\n" +
                       "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n" +
                       "json\r\n" +
                       "--" + boundary + "\r\n" +
                       "Content-Disposition: form-data; name=\"prompt\"\r\n\r\n" +
                       "Hội thoại tiếng Việt: tủ lạnh, ngăn mát, ngăn đông, tiết kiệm điện, khóa trẻ em, thời tiết, nấu ăn, bài hát.\r\n" +
                       "--" + boundary + "\r\n" +
                       "Content-Disposition: form-data; name=\"temperature\"\r\n\r\n" +
                       "0.0\r\n" +
                       "--" + boundary + "\r\n" +
                       "Content-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\n" +
                       "Content-Type: audio/wav\r\n\r\n";
    String part_end = String("\r\n--") + boundary + "--\r\n";

    uint32_t pcm_bytes = sample_count * sizeof(int16_t);
    uint32_t wav_payload_size = 44 + pcm_bytes;
    size_t total_len = part_hdrs.length() + wav_payload_size + part_end.length();

    String body = "";
    bool req_ok = false;

    for (int attempt = 0; attempt < 2; attempt++) {
        if (!ensure_connected_locked()) {
            Serial.println("[GROQ] Failed to connect to api.groq.com:443");
            xSemaphoreGive(s_tls_mutex);
            return false;
        }

        String req_head = String("POST /openai/v1/audio/transcriptions HTTP/1.1\r\n") +
                          "Host: " + GROQ_HOST + "\r\n" +
                          "Authorization: Bearer " + GROQ_API_KEY + "\r\n" +
                          "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n" +
                          "Content-Length: " + String(total_len) + "\r\n" +
                          "Connection: keep-alive\r\n\r\n" +
                          part_hdrs;

        size_t written = s_tls_client.write((const uint8_t *)req_head.c_str(), req_head.length());
        if (written == 0) {
            // Stale keep-alive socket closed by server, reconnect immediately on attempt 1
            s_tls_client.stop();
            continue;
        }

        // Write 44-byte WAV header
        uint8_t wav_hdr[44];
        build_wav_header(wav_hdr, pcm_bytes, 16000, 1, 16);
        s_tls_client.write(wav_hdr, 44);

        // Stream PCM audio samples in fast 4096-byte chunks
        const uint8_t *ptr = (const uint8_t *)samples;
        size_t remaining = pcm_bytes;
        while (remaining > 0 && s_tls_client.connected()) {
            size_t chunk = (remaining > 4096) ? 4096 : remaining;
            size_t w = s_tls_client.write(ptr, chunk);
            if (w == 0) break;
            ptr += w;
            remaining -= w;
        }

        s_tls_client.write((const uint8_t *)part_end.c_str(), part_end.length());

        req_ok = read_http_json_response(s_tls_client, body, 7000);
        if (req_ok) break;
        s_tls_client.stop();
    }

    xSemaphoreGive(s_tls_mutex);

    if (!req_ok) {
        Serial.printf("[GROQ] Whisper STT failed: %s\n", body.c_str());
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[GROQ] STT JSON parse failed: %s\n", err.c_str());
        return false;
    }

    const char *text = doc["text"];
    if (!text || strlen(text) == 0) {
        return false;
    }

    strncpy(out_text, text, max_out_len - 1);
    out_text[max_out_len - 1] = '\0';
    Serial.printf("[GROQ] STT Done in %lu ms -> \"%s\"\n", millis() - t_start, out_text);
    return true;
}

// 2. Chat Completion via Groq LLM (Reuses same Keep-Alive TLS Socket -> ~0ms handshake!)
static bool chat_completion_with_model(const char *model_name, const char *user_prompt, char *out_answer, size_t max_out_len, int temp_mat, int temp_dong, const char *time_context = nullptr) {
    ensure_mutex();
    if (xSemaphoreTake(s_tls_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return false;

    unsigned long t_start = millis();

    char sys_prompt[1200];
    const char *tc = (time_context && strlen(time_context) > 0) ? time_context : "Thứ Hai ngày 28/09/2026 lúc 16:30";
    snprintf(sys_prompt, sizeof(sys_prompt),
             "Bạn là MiniMazing, trợ lý giọng nói AI cực kỳ thông minh, hiểu biết sâu rộng và duyên dáng trên tủ lạnh gia đình.\n"
             "BỐI CẢNH THỜI GIAN & THIẾT BỊ:\n"
             "- Thời gian thực tại Việt Nam: %s\n"
             "- Nhiệt độ tủ lạnh: ngăn mát %d độ C, ngăn đông %d độ C.\n"
             "QUY TẮC GIAO TIẾP VÀNG:\n"
             "1. Giao tiếp 100%% bằng tiếng Việt tự nhiên, xưng em hoặc MiniMazing, gọi anh chị hoặc bạn. Tuyệt đối không xuất thẻ <think> hay ký tự markdown (*, #, _, -).\n"
             "2. HÁT HÒ: Khi người dùng yêu cầu hát hoặc hát bài gì đó, hãy cất giọng hát ngay 4 đến 6 câu thơ hoặc lời bài hát vui tươi, ngọt ngào, có vần điệu nhịp nhàng.\n"
             "3. THỜI GIAN & LỊCH: Khi hỏi về thứ, ngày, tháng, năm, giờ giấc, hãy trả lời chính xác theo thông tin thời gian thực ở trên.\n"
             "4. HỎI ĐÁP ĐỜI SỐNG & TỦ LẠNH: Luôn trả lời thông minh, đầy đủ ý, hữu ích trong 2-3 câu vừa phải (khoảng 30-50 từ) để loa phát âm thanh mượt mà truyền cảm.",
             tc, temp_mat, temp_dong);

    JsonDocument req_doc;
    req_doc["model"] = model_name;
    JsonArray messages = req_doc["messages"].to<JsonArray>();

    JsonObject msg_sys = messages.add<JsonObject>();
    msg_sys["role"] = "system";
    msg_sys["content"] = sys_prompt;

    JsonObject msg_user = messages.add<JsonObject>();
    msg_user["role"] = "user";
    msg_user["content"] = user_prompt;

    req_doc["max_tokens"] = 180;
    req_doc["temperature"] = 0.7;

    String json_body;
    serializeJson(req_doc, json_body);

    String body = "";
    bool req_ok = false;

    for (int attempt = 0; attempt < 2; attempt++) {
        if (!ensure_connected_locked()) {
            xSemaphoreGive(s_tls_mutex);
            return false;
        }

        String req = String("POST /openai/v1/chat/completions HTTP/1.1\r\n") +
                     "Host: " + GROQ_HOST + "\r\n" +
                     "Authorization: Bearer " + GROQ_API_KEY + "\r\n" +
                     "Content-Type: application/json\r\n" +
                     "Content-Length: " + String(json_body.length()) + "\r\n" +
                     "Connection: keep-alive\r\n\r\n" +
                     json_body;

        size_t w = s_tls_client.write((const uint8_t *)req.c_str(), req.length());
        if (w == 0) {
            s_tls_client.stop();
            continue;
        }

        req_ok = read_http_json_response(s_tls_client, body, 7000);
        if (req_ok) break;
        s_tls_client.stop();
    }

    xSemaphoreGive(s_tls_mutex);

    if (!req_ok) {
        Serial.printf("[GROQ] LLM (%s) failed: %s\n", model_name, body.substring(0, 200).c_str());
        return false;
    }

    JsonDocument res_doc;
    DeserializationError err = deserializeJson(res_doc, body);
    if (err) {
        Serial.printf("[GROQ] LLM JSON parse failed: %s\n", err.c_str());
        return false;
    }

    const char *reply = res_doc["choices"][0]["message"]["content"];
    if (!reply || strlen(reply) == 0) {
        return false;
    }

    strncpy(out_answer, reply, max_out_len - 1);
    out_answer[max_out_len - 1] = '\0';
    Serial.printf("[GROQ] LLM (%s) Done in %lu ms -> \"%s\"\n", model_name, millis() - t_start, out_answer);
    return true;
}

bool chat_completion(const char *user_prompt, char *out_answer, size_t max_out_len, int temp_mat, int temp_dong, const char *time_context) {
    if (!user_prompt || strlen(user_prompt) == 0 || !out_answer) return false;
    if (strlen(GROQ_API_KEY) == 0) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

#ifdef GROQ_LLM_MODEL
    const char *primary_model = GROQ_LLM_MODEL;
#else
    const char *primary_model = "qwen/qwen3.8-27b";
#endif

    if (chat_completion_with_model(primary_model, user_prompt, out_answer, max_out_len, temp_mat, temp_dong, time_context)) {
        return true;
    }

#ifdef GROQ_LLM_FAST
    if (strcmp(primary_model, GROQ_LLM_FAST) != 0) {
        Serial.printf("[GROQ] Retrying with ultra-fast model %s...\n", GROQ_LLM_FAST);
        return chat_completion_with_model(GROQ_LLM_FAST, user_prompt, out_answer, max_out_len, temp_mat, temp_dong);
    }
#endif
    return false;
}

} // namespace GroqAI

