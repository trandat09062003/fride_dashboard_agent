#include "zing_player.h"
#include "config_ai.h"
#include "es8311_driver.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include "minimp3.h"

namespace ZingPlayer {

static volatile bool s_is_playing = false;
static volatile bool s_stop_requested = false;

static mp3dec_t s_mp3d;
static mp3dec_frame_info_t s_info;
static int16_t s_pcm_frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int16_t s_stereo_frame[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];

static const size_t STREAM_BUF_SIZE = 64 * 1024; // 64KB in PSRAM
static uint8_t *s_stream_buf = nullptr;

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

void init() {
    if (!s_stream_buf) {
        s_stream_buf = (uint8_t *)ps_malloc(STREAM_BUF_SIZE);
        if (!s_stream_buf) {
            s_stream_buf = (uint8_t *)malloc(32 * 1024);
        }
    }
    Serial.printf("[ZING] ZingPlayer initialized (Buffer: %s)\n", s_stream_buf ? "OK" : "FAILED");
}

bool is_playing() {
    return s_is_playing;
}

void stop() {
    if (s_is_playing) {
        Serial.println("[ZING] Stop playback requested.");
        s_stop_requested = true;
    }
}

bool search_song(const char *query, SongInfo &out_info) {
    memset(&out_info, 0, sizeof(SongInfo));
    if (!query || strlen(query) == 0) return false;
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[ZING] WiFi not connected!");
        return false;
    }

    String url = String("http://") + ZING_PROXY_HOST + ":" + String(ZING_PROXY_PORT) + "/api/song?q=" + url_encode(query);
    Serial.printf("[ZING] Searching song via: %s\n", url.c_str());

    HTTPClient http;
    http.begin(url);
    http.setTimeout(8000);
    int code = http.GET();

    if (code != 200) {
        Serial.printf("[ZING] Search failed with HTTP code: %d\n", code);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("[ZING] JSON deserialize error: %s\n", err.c_str());
        return false;
    }

    const char *status = doc["status"];
    if (!status || strcmp(status, "ok") != 0) {
        Serial.println("[ZING] Song search returned non-ok status");
        return false;
    }

    strncpy(out_info.title, doc["title"] | "Không tên", sizeof(out_info.title) - 1);
    strncpy(out_info.artist, doc["artist"] | "Nhiều nghệ sĩ", sizeof(out_info.artist) - 1);
    strncpy(out_info.song_id, doc["songId"] | "", sizeof(out_info.song_id) - 1);
    out_info.duration = doc["duration"] | 0;
    out_info.found = (strlen(out_info.song_id) > 0);

    Serial.printf("[ZING] Found song: \"%s\" - %s (ID: %s)\n", out_info.title, out_info.artist, out_info.song_id);
    return out_info.found;
}

bool play_song_by_query(const char *query,
                        void (*on_level_cb)(int level),
                        void (*status_cb)(const char *msg)) {
    if (!query || strlen(query) == 0) return false;
    if (WiFi.status() != WL_CONNECTED) {
        if (status_cb) status_cb("Lỗi: Chưa kết nối Wi-Fi!");
        return false;
    }

    init();
    if (!s_stream_buf) {
        if (status_cb) status_cb("Lỗi: Không đủ bộ nhớ đệm âm thanh!");
        return false;
    }

    if (status_cb) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Đang tìm bài \"%s\" trên Zing MP3...", query);
        status_cb(msg);
    }

    SongInfo info;
    if (!search_song(query, info)) {
        if (status_cb) {
            char msg[160];
            snprintf(msg, sizeof(msg), "Không tìm thấy bài \"%s\" hoặc bài VIP!", query);
            status_cb(msg);
        }
        return false;
    }

    if (status_cb) {
        char msg[160];
        snprintf(msg, sizeof(msg), "🎵 Đang phát: %s - %s", info.title, info.artist);
        status_cb(msg);
    }

    // Connect to proxy stream endpoint
    WiFiClient client;
    client.setTimeout(12);

    Serial.printf("[ZING] Connecting to proxy stream: %s:%d...\n", ZING_PROXY_HOST, ZING_PROXY_PORT);
    if (!client.connect(ZING_PROXY_HOST, ZING_PROXY_PORT)) {
        Serial.println("[ZING] Connection to proxy stream failed!");
        if (status_cb) status_cb("Lỗi: Không thể kết nối tới Zing Proxy Server!");
        return false;
    }

    String req = String("GET /api/stream?id=") + info.song_id + " HTTP/1.1\r\n" +
                 "Host: " + ZING_PROXY_HOST + ":" + String(ZING_PROXY_PORT) + "\r\n" +
                 "User-Agent: ESP32S3_SmartFridge\r\n" +
                 "Connection: close\r\n\r\n";
    client.print(req);
    client.flush();

    // Wait for response headers
    unsigned long start_ms = millis();
    while (client.connected() && client.available() == 0) {
        if (millis() - start_ms > 10000) {
            Serial.println("[ZING] Stream response timeout!");
            client.stop();
            if (status_cb) status_cb("Lỗi: Máy chủ stream phản hồi quá lâu!");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Verify HTTP status line
    String status_line = client.readStringUntil('\n');
    if (!status_line.startsWith("HTTP/1.1 200") && !status_line.startsWith("HTTP/1.0 200")) {
        Serial.printf("[ZING] Stream returned non-200: %s\n", status_line.c_str());
        client.stop();
        if (status_cb) status_cb("Lỗi: Không thể tải luồng MP3!");
        return false;
    }

    // Skip headers until empty line (\r\n\r\n)
    while (client.connected() || client.available()) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) break;
    }

    Serial.println("[ZING] Stream started! Decoding MP3...");

    s_is_playing = true;
    s_stop_requested = false;

    mp3dec_init(&s_mp3d);
    ES8311_Audio::mute(false);
    vTaskDelay(pdMS_TO_TICKS(30)); // Allow bypass capacitor ramp-up

    size_t buf_len = 0;
    size_t offset = 0;
    int current_hz = 0;
    uint32_t frame_count = 0;
    unsigned long last_data_time = millis();

    while (s_is_playing && !s_stop_requested) {
        // 1. Fill buffer from network stream
        while (client.available() > 0 && (buf_len < STREAM_BUF_SIZE)) {
            size_t to_read = STREAM_BUF_SIZE - buf_len;
            size_t bytes_in = client.readBytes(s_stream_buf + buf_len, to_read);
            if (bytes_in > 0) {
                buf_len += bytes_in;
                last_data_time = millis();
            }
        }

        // 2. Check if stream ended
        if (!client.connected() && client.available() == 0 && (buf_len - offset < 32)) {
            Serial.println("[ZING] End of song stream reached.");
            break;
        }

        // Check for network freeze timeout
        if (millis() - last_data_time > 8000 && (buf_len - offset < 32)) {
            Serial.println("[ZING] Stream read timeout.");
            break;
        }

        // 3. Compact buffer when offset gets large
        if (offset > 16384) {
            size_t remaining = buf_len - offset;
            if (remaining > 0) {
                memmove(s_stream_buf, s_stream_buf + offset, remaining);
            }
            buf_len = remaining;
            offset = 0;
        }

        // 4. Need minimum data to decode frame
        if (buf_len - offset < 256) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // 5. Decode single MP3 frame
        int samples = mp3dec_decode_frame(&s_mp3d, s_stream_buf + offset, buf_len - offset, s_pcm_frame, &s_info);
        if (s_info.frame_bytes <= 0) {
            // Corrupt or boundary frame, skip byte
            offset++;
            continue;
        }
        offset += s_info.frame_bytes;

        if (samples > 0) {
            frame_count++;

            // Adjust I2S sample rate dynamically to match MP3 (e.g. 44100 Hz, 48000 Hz)
            if (s_info.hz > 0 && s_info.hz != current_hz) {
                current_hz = s_info.hz;
                Serial.printf("[ZING] Setting I2S clock to %d Hz (%d channels)\n", s_info.hz, s_info.channels);
                i2s_set_clk(I2S_NUM_0, s_info.hz, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
            }

            // Convert mono to stereo & apply 75% volume scaling to avoid distortion on SC8002B
            if (s_info.channels == 1) {
                for (int i = 0; i < samples; i++) {
                    int32_t val = (int32_t)s_pcm_frame[i] * 3 / 4;
                    s_stereo_frame[i * 2]     = (int16_t)val;
                    s_stereo_frame[i * 2 + 1] = (int16_t)val;
                }
            } else {
                for (int i = 0; i < samples; i++) {
                    int32_t l = (int32_t)s_pcm_frame[i * 2] * 3 / 4;
                    int32_t r = (int32_t)s_pcm_frame[i * 2 + 1] * 3 / 4;
                    s_stereo_frame[i * 2]     = (int16_t)l;
                    s_stereo_frame[i * 2 + 1] = (int16_t)r;
                }
            }

            // Send stereo PCM to I2S DAC
            size_t bytes_written = 0;
            i2s_write(I2S_NUM_0, s_stereo_frame, samples * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);

            // Compute audio RMS & animate spectrum UI
            if (on_level_cb && (frame_count % 3 == 0)) {
                int64_t sq = 0;
                for (int i = 0; i < samples; i++) {
                    int32_t s = s_pcm_frame[i * s_info.channels];
                    sq += s * s;
                }
                int32_t chunk_rms = (int32_t)sqrtf((float)(sq / samples));
                int level = map(chunk_rms, 100, 6000, 0, 100);
                if (level < 0) level = 0;
                if (level > 100) level = 100;
                on_level_cb(level);
            }
        }

        // Allow FreeRTOS task yield to prevent Watchdog timeout
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    client.stop();
    ES8311_Audio::mute(true);
    s_is_playing = false;

    if (on_level_cb) on_level_cb(0);

    if (s_stop_requested) {
        Serial.println("[ZING] Playback stopped by user.");
        if (status_cb) status_cb("Đã dừng phát nhạc.");
    } else {
        Serial.println("[ZING] Playback completed.");
        if (status_cb) status_cb("Đã phát xong bài hát!");
    }

    return (frame_count > 0);
}

} // namespace ZingPlayer
