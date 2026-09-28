#include "gemini_client.h"
#include "config_ai.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace GeminiAI {

void init() {
    Serial.println("[GEMINI] Google Gemini AI Client Initialized.");
}

bool chat_completion(const char *user_prompt, char *out_answer, size_t max_out_len, int temp_mat, int temp_dong) {
    if (!user_prompt || strlen(user_prompt) == 0 || !out_answer) return false;
    if (strlen(GEMINI_API_KEY) == 0) {
        Serial.println("[GEMINI] GEMINI_API_KEY is empty!");
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[GEMINI] WiFi not connected!");
        return false;
    }

    Serial.printf("[GEMINI] Requesting Google Gemini API (Model: %s)...\n", GEMINI_MODEL);

    // Build System Prompt
    char sys_prompt[800];
    snprintf(sys_prompt, sizeof(sys_prompt),
        "Bạn là MiniMazing, trợ lý giọng nói thông minh cho tủ lạnh gia đình. "
        "QUY TẮC: Chỉ trả lời 100%% bằng tiếng Việt có dấu tự nhiên, lễ phép (xưng em, gọi anh chị/bạn), không dùng ký tự markdown (*, #, -). "
        "Trả lời ngắn gọn, trọn ý trong 1 đến 2 câu (khoảng 25 đến 45 từ) để phát âm nhanh. "
        "Trạng thái tủ lạnh: ngăn mát %d°C, ngăn đông %d°C. "
        "Nếu người dùng yêu cầu hát, hãy vui vẻ hát ngay 3-4 câu ngắn vui tươi.",
        temp_mat, temp_dong);

    // Build Request JSON using ArduinoJson 7
    JsonDocument doc;
    JsonArray sysParts = doc["systemInstruction"]["parts"].to<JsonArray>();
    JsonObject sysPart = sysParts.add<JsonObject>();
    sysPart["text"] = sys_prompt;

    JsonArray contents = doc["contents"].to<JsonArray>();
    JsonObject contentObj = contents.add<JsonObject>();
    JsonArray parts = contentObj["parts"].to<JsonArray>();
    JsonObject partObj = parts.add<JsonObject>();
    partObj["text"] = user_prompt;

    JsonObject genConfig = doc["generationConfig"].to<JsonObject>();
    genConfig["maxOutputTokens"] = 200;
    genConfig["temperature"] = 0.6;

    String jsonPayload;
    serializeJson(doc, jsonPayload);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(7);

    HTTPClient http;
    String url = String("https://generativelanguage.googleapis.com/v1beta/models/") + GEMINI_MODEL + ":generateContent?key=" + GEMINI_API_KEY;

    if (!http.begin(client, url)) {
        Serial.println("[GEMINI] HTTP begin failed!");
        return false;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "MiniMazing-ESP32/1.0");
    http.setTimeout(7000);

    int httpCode = http.POST(jsonPayload);
    Serial.printf("[GEMINI] HTTP POST Response Code: %d\n", httpCode);

    if (httpCode != HTTP_CODE_OK && httpCode != 200) {
        String err_body = http.getString();
        Serial.printf("[GEMINI] Error (%d): %s\n", httpCode, err_body.substring(0, 250).c_str());
        http.end();
        return false;
    }

    String response = http.getString();
    http.end();

    // Parse JSON Body
    JsonDocument resDoc;
    DeserializationError err = deserializeJson(resDoc, response);
    if (err) {
        Serial.printf("[GEMINI] JSON parse error: %s\n", err.c_str());
        return false;
    }

    const char *reply = resDoc["candidates"][0]["content"]["parts"][0]["text"];
    if (!reply || strlen(reply) == 0) {
        Serial.println("[GEMINI] No content text in response!");
        return false;
    }

    // Clean Markdown markers (*, #, _, `, etc.) for clean speech & display
    size_t j = 0;
    for (size_t i = 0; reply[i] != '\0' && j < max_out_len - 1; i++) {
        if (reply[i] == '*' || reply[i] == '#' || reply[i] == '`' || reply[i] == '_') {
            continue;
        }
        out_answer[j++] = reply[i];
    }
    out_answer[j] = '\0';

    Serial.printf("[GEMINI] Reply (%d chars): %s\n", (int)strlen(out_answer), out_answer);
    return true;
}

} // namespace GeminiAI
