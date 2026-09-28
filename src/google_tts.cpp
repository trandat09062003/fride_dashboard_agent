#include "google_tts.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <driver/i2s.h>
#include <vector>
#include "es8311_driver.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

static String url_encode(const char *str) {
    String encoded = "";
    char c;
    while ((c = *str++)) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += "%20";
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
            encoded += buf;
        }
    }
    return encoded;
}

namespace GoogleTTS {

// Static persistent decoder buffers (PSRAM allocated)
static const size_t max_mp3_size = 384 * 1024; // 384KB in PSRAM (~35s of audio)
static uint8_t *mp3_buf = nullptr;
static mp3dec_t mp3d;
static mp3dec_frame_info_t info;
static int16_t pcm_frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int16_t stereo_frame[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];

// Split text into natural, sentence-aligned chunks (<150 UTF-8 bytes) so Google TTS never returns HTTP 400
static void split_into_chunks(const char *text, std::vector<String> &chunks, size_t max_len = 150) {
    if (!text || strlen(text) == 0) return;

    // Clean text: strip markdown characters
    String clean_text = "";
    clean_text.reserve(strlen(text));
    for (size_t i = 0; text[i] != '\0'; i++) {
        char ch = text[i];
        if (ch == '*' || ch == '#' || ch == '_' || ch == '~' || ch == '`' || ch == '\"' || ch == '\\') {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            clean_text += ' ';
        } else {
            clean_text += ch;
        }
    }

    String current = "";
    int len = clean_text.length();
    int i = 0;

    while (i < len) {
        // Skip leading spaces for new chunk
        while (i < len && clean_text[i] == ' ') i++;
        if (i >= len) break;

        // Find next word
        int word_start = i;
        while (i < len && clean_text[i] != ' ') i++;
        String word = clean_text.substring(word_start, i);

        if (current.length() + word.length() + 1 > max_len) {
            current.trim();
            if (current.length() > 0) {
                chunks.push_back(current);
            }
            current = word + " ";
        } else {
            current += word + " ";
            char last_c = word.charAt(word.length() - 1);
            if (last_c == '.' || last_c == '!' || last_c == '?' || last_c == ';' || last_c == ':') {
                current.trim();
                if (current.length() > 0) {
                    chunks.push_back(current);
                }
                current = "";
            } else if (last_c == ',' && current.length() >= 80) {
                current.trim();
                if (current.length() > 0) {
                    chunks.push_back(current);
                }
                current = "";
            }
        }
    }

    current.trim();
    if (current.length() > 0) {
        chunks.push_back(current);
    }
}

static WiFiClient s_tts_client;

static bool internal_speak_single_chunk(const char *chunk_utf8, void (*on_level_cb)(int level), int &current_hz) {
    if (!chunk_utf8 || strlen(chunk_utf8) == 0) return true;

    unsigned long t_req = millis();
    Serial.printf("[TTS] Voice chunk (%d chars): \"%s\"\n", (int)strlen(chunk_utf8), chunk_utf8);

    const char *host = "translate.google.com";
    String path = "/translate_tts?ie=UTF-8&tl=vi&client=tw-ob&q=" + url_encode(chunk_utf8);

    bool header_ok = false;

    // Reset client for a clean, deterministic HTTP connection per chunk
    s_tts_client.stop();
    s_tts_client.setTimeout(5);

    for (int attempt = 0; attempt < 3; attempt++) {
        if (!s_tts_client.connect(host, 80)) {
            Serial.printf("[TTS] Connection attempt %d to %s:80 failed!\n", attempt + 1, host);
            s_tts_client.stop();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Use HTTP/1.0 with Connection: close to guarantee unchunked, pure raw MP3 stream!
        String req = "GET " + path + " HTTP/1.0\r\n" +
                     "Host: " + host + "\r\n" +
                     "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\r\n" +
                     "Accept: */*\r\n" +
                     "Connection: close\r\n\r\n";

        if (s_tts_client.write((const uint8_t *)req.c_str(), req.length()) == 0) {
            s_tts_client.stop();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Wait for response headers
        unsigned long start_wait = millis();
        while (s_tts_client.connected() && s_tts_client.available() == 0) {
            if (millis() - start_wait > 4000) break;
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        if (s_tts_client.available() == 0) {
            s_tts_client.stop();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Parse status line
        String status_line = s_tts_client.readStringUntil('\n');
        status_line.trim();
        if (!status_line.startsWith("HTTP/1.0 200") && !status_line.startsWith("HTTP/1.1 200")) {
            Serial.printf("[TTS] Non-200 HTTP status: %s\n", status_line.c_str());
            s_tts_client.stop();
            return false;
        }

        // Skip headers until empty line (\r\n\r\n)
        while (s_tts_client.connected() || s_tts_client.available()) {
            String line = s_tts_client.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) {
                header_ok = true;
                break;
            }
        }

        if (header_ok) break;
    }

    if (!header_ok) {
        s_tts_client.stop();
        return false;
    }

    // Step 1: Download full MP3 data into PSRAM buffer first (~300-500ms total)
    // This completely decouples network jitter/socket stalls from audio I2S DMA playback!
    size_t mp3_len = 0;
    unsigned long start_dl = millis();
    unsigned long last_data_ms = millis();

    while (s_tts_client.connected() || s_tts_client.available()) {
        int avail = s_tts_client.available();
        if (avail > 0) {
            int to_read = avail;
            if (mp3_len + to_read > max_mp3_size) to_read = max_mp3_size - mp3_len;
            if (to_read <= 0) break;
            int r = s_tts_client.readBytes((char *)(mp3_buf + mp3_len), to_read);
            if (r > 0) {
                mp3_len += r;
                last_data_ms = millis();
            }
        } else {
            if (!s_tts_client.connected()) break;
            if (millis() - last_data_ms > 1500) break;
            vTaskDelay(pdMS_TO_TICKS(3));
        }
    }
    s_tts_client.stop();

    Serial.printf("[TTS] Downloaded %u bytes of MP3 in %lu ms\n", (unsigned int)mp3_len, millis() - start_dl);
    if (mp3_len < 128) {
        Serial.println("[TTS] Insufficient MP3 data received!");
        return false;
    }

    // Step 2: Decode and play the entire buffered MP3 continuously with ZERO network stalls
    mp3dec_init(&mp3d);
    size_t decode_offset = 0;
    int frame_count = 0;
    unsigned long start_play_ms = millis();

    while (decode_offset < mp3_len) {
        int samples = mp3dec_decode_frame(&mp3d, mp3_buf + decode_offset, mp3_len - decode_offset, pcm_frame, &info);
        if (info.frame_bytes > 0) {
            decode_offset += info.frame_bytes;

            if (samples > 0) {
                frame_count++;

                if (info.hz > 0 && info.hz != current_hz) {
                    current_hz = info.hz;
                    i2s_set_clk(I2S_NUM_0, info.hz, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
                }

                // Volume 70% for loud, crystal-clear voice without distortion
                if (info.channels == 1) {
                    for (int i = 0; i < samples; i++) {
                        int32_t val = (int32_t)pcm_frame[i] * 70 / 100;
                        stereo_frame[i * 2]     = (int16_t)val;
                        stereo_frame[i * 2 + 1] = (int16_t)val;
                    }
                } else {
                    for (int i = 0; i < samples; i++) {
                        int32_t l = (int32_t)pcm_frame[i * 2] * 70 / 100;
                        int32_t r = (int32_t)pcm_frame[i * 2 + 1] * 70 / 100;
                        stereo_frame[i * 2]     = (int16_t)l;
                        stereo_frame[i * 2 + 1] = (int16_t)r;
                    }
                }

                size_t bytes_written = 0;
                i2s_write(I2S_NUM_0, stereo_frame, samples * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);

                if (on_level_cb && (frame_count % 2 == 0)) {
                    int64_t sq = 0;
                    for (int i = 0; i < samples; i++) {
                        int32_t s = pcm_frame[i * info.channels];
                        sq += s * s;
                    }
                    int32_t chunk_rms = (int32_t)sqrtf((float)(sq / samples));
                    int level = map(chunk_rms, 100, 5000, 0, 100);
                    if (level < 0) level = 0;
                    if (level > 100) level = 100;
                    on_level_cb(level);
                }
            }
        } else {
            // End of stream or padding: if less than 128 bytes remain, finish cleanly
            if (mp3_len - decode_offset < 128) {
                break;
            }
            // Skip 1 byte to find next frame sync word
            decode_offset++;
        }
    }

    Serial.printf("[TTS] Chunk playback complete: %d frames played in %lu ms\n", frame_count, millis() - start_play_ms);
    return (frame_count > 0);
}

static bool internal_speak(const char *text_utf8, void (*on_level_cb)(int level)) {
    if (!text_utf8 || strlen(text_utf8) == 0) return false;

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[TTS] WiFi not connected! Skipping Google TTS stream.");
        return false;
    }

    if (!mp3_buf) {
        mp3_buf = (uint8_t *)ps_malloc(max_mp3_size);
        if (!mp3_buf) {
            mp3_buf = (uint8_t *)malloc(64 * 1024);
        }
    }
    if (!mp3_buf) {
        Serial.println("[TTS] Failed to allocate mp3_buf!");
        return false;
    }

    std::vector<String> chunks;
    split_into_chunks(text_utf8, chunks, 150);
    Serial.printf("[TTS] Total text len: %d bytes | Split into %d chunks\n", (int)strlen(text_utf8), (int)chunks.size());

    if (chunks.empty()) return false;

    ES8311_Audio::mute(false);
    vTaskDelay(pdMS_TO_TICKS(15));

    int current_hz = 0;
    bool any_success = false;
    for (size_t i = 0; i < chunks.size(); i++) {
        bool ok = internal_speak_single_chunk(chunks[i].c_str(), on_level_cb, current_hz);
        if (ok) {
            any_success = true;
        }
    }

    // Clear DMA buffer, shut down PA amplifier to prevent idle hum/beep, and restore 16kHz mic clock
    i2s_zero_dma_buffer(I2S_NUM_0);
    ES8311_Audio::mute(true);
    i2s_set_clk(I2S_NUM_0, AUDIO_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
    vTaskDelay(pdMS_TO_TICKS(5));
    if (on_level_cb) on_level_cb(-1);

    return any_success;
}

struct TTSParams {
    char text[1536];
    void (*on_level_cb)(int level);
    SemaphoreHandle_t done_sem;
    bool result;
};

static TTSParams g_tts_params;

static void tts_freertos_task(void *pvParam) {
    TTSParams *p = (TTSParams *)pvParam;
    p->result = internal_speak(p->text, p->on_level_cb);
    if (p->done_sem) xSemaphoreGive(p->done_sem);
    vTaskDelete(NULL);
}

bool speak(const char *text_utf8, void (*on_level_cb)(int level)) {
    if (!text_utf8 || strlen(text_utf8) == 0) return false;
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[TTS] WiFi not connected! Skipping Google TTS stream.");
        return false;
    }

    strncpy(g_tts_params.text, text_utf8, sizeof(g_tts_params.text) - 1);
    g_tts_params.text[sizeof(g_tts_params.text) - 1] = '\0';
    g_tts_params.on_level_cb = on_level_cb;
    g_tts_params.done_sem = xSemaphoreCreateBinary();
    g_tts_params.result = false;

    BaseType_t res = xTaskCreatePinnedToCore(
        tts_freertos_task,
        "tts_worker",
        36864,   // 36 KB dedicated stack
        &g_tts_params,
        2,       // Priority
        NULL,
        1        // Core 1
    );

    if (res != pdPASS) {
        Serial.println("[TTS] Failed to create TTS worker task, running in current task!");
        if (g_tts_params.done_sem) vSemaphoreDelete(g_tts_params.done_sem);
        return internal_speak(text_utf8, on_level_cb);
    }

    // Wait up to 90 seconds for lengthy multi-sentence speech to finish
    xSemaphoreTake(g_tts_params.done_sem, pdMS_TO_TICKS(90000));
    vSemaphoreDelete(g_tts_params.done_sem);

    return g_tts_params.result;
}

} // namespace GoogleTTS
