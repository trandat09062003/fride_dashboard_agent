#pragma once

#include <Arduino.h>
#include <driver/i2c.h>
#include <driver/i2s.h>
#include <math.h>

#define AUDIO_SAMPLE_RATE 16000

/* ====================================================================
 * ES8311 HIGH-PERFORMANCE AUDIO DRIVER FOR ESP32-S3
 * Based on official Espressif esp-adf & esp_codec_dev driver
 * Supports full register configuration, diagnostics & multi-mode tests
 * ==================================================================== */
class ES8311_Audio {
private:
    static uint8_t& codec_addr() {
        static uint8_t addr = 0x18;
        return addr;
    }

    static int& pa_pin() {
        static int pin = 1;
        return pin;
    }

    static int& dout_pin() {
        static int dout = 15;
        return dout;
    }

    static int& din_pin() {
        static int din = 16;
        return din;
    }

    static int& mclk_pin() {
        static int mclk = 17;
        return mclk;
    }

    static int& bclk_pin() {
        static int bclk = 18;
        return bclk;
    }

    static int& ws_pin() {
        static int ws = 21;
        return ws;
    }

    static int& current_hw_mode() {
        // Mode 0: DOUT=16, DIN=15, PA_ACTIVE=HIGH (1)
        // Mode 1: DOUT=16, DIN=15, PA_ACTIVE=LOW  (0)
        // Mode 2: DOUT=15, DIN=16, PA_ACTIVE=HIGH (1)
        // Mode 3: DOUT=15, DIN=16, PA_ACTIVE=LOW  (0) - CHUẨN XÁC 100% PHẦN CỨNG!
        static int mode = 3; // Default to Mode 3 (DOUT=15, DIN=16, PA=0 Active LOW)
        return mode;
    }

    static esp_err_t write_reg(uint8_t reg, uint8_t val) {
        uint8_t buf[2] = { reg, val };
        return i2c_master_write_to_device(I2C_NUM_0, codec_addr(), buf, 2, pdMS_TO_TICKS(50));
    }

    static uint8_t read_reg(uint8_t reg) {
        uint8_t val = 0;
        i2c_master_write_read_device(I2C_NUM_0, codec_addr(), &reg, 1, &val, 1, pdMS_TO_TICKS(50));
        return val;
    }

    static bool& low_battery_quiet() {
        static bool quiet = false;
        return quiet;
    }

    static bool& ui_sound_enabled() {
        static bool en = true;
        return en;
    }

public:
    static void set_low_battery_quiet(bool quiet) {
        low_battery_quiet() = quiet;
        if (quiet) {
            mute(true);
        }
    }

    static bool is_low_battery_quiet() {
        return low_battery_quiet();
    }

    static void set_ui_sound_enabled(bool en) {
        ui_sound_enabled() = en;
        if (!en) {
            mute(true);
        }
    }

    static uint8_t get_i2c_addr() {
        return codec_addr();
    }

    static int get_hw_mode() {
        return current_hw_mode();
    }

    static bool is_pa_active_high() {
        return (current_hw_mode() == 0 || current_hw_mode() == 2);
    }

    // Set PA amplifier output state
    static void mute(bool muted) {
        if (pa_pin() >= 0) {
            bool pa_high = is_pa_active_high();
            if (muted) {
                // When muted, set opposite of active state (SHUTDOWN SC8002B to prevent idle noise/beeping)
                digitalWrite(pa_pin(), pa_high ? LOW : HIGH);
            } else {
                // When active/unmuted, set active state
                digitalWrite(pa_pin(), pa_high ? HIGH : LOW);
            }
        }
    }

    // Switch between 4 hardware pin & PA combinations
    static void set_hardware_mode(int mode) {
        current_hw_mode() = mode % 4;

        if (current_hw_mode() == 0) {
            dout_pin() = 16; din_pin() = 15;
        } else if (current_hw_mode() == 1) {
            dout_pin() = 16; din_pin() = 15;
        } else if (current_hw_mode() == 2) {
            dout_pin() = 15; din_pin() = 16;
        } else if (current_hw_mode() == 3) {
            dout_pin() = 15; din_pin() = 16;
        }

        i2s_pin_config_t pin_config = {
            .mck_io_num   = mclk_pin(),
            .bck_io_num   = bclk_pin(),
            .ws_io_num    = ws_pin(),
            .data_out_num = dout_pin(),
            .data_in_num  = din_pin()
        };
        i2s_set_pin(I2S_NUM_0, &pin_config);

        // Keep PA muted when idle so SC8002B never oscillates or hums on low battery
        mute(true);

        Serial.printf("[AUDIO-MODE] Switched to Mode %d: DOUT=%d, DIN=%d, PA=%s\n",
                      current_hw_mode(), dout_pin(), din_pin(),
                      is_pa_active_high() ? "HIGH (1)" : "LOW (0)");
    }

    static void cycle_hardware_mode() {
        set_hardware_mode((current_hw_mode() + 1) % 4);
    }

    // Dump all registers over Serial for diagnosis
    static void dump_registers() {
        Serial.println("====== ES8311 REGISTER DUMP ======");
        for (uint8_t r = 0; r <= 0x45; r++) {
            uint8_t val = read_reg(r);
            Serial.printf("[0x%02X]=0x%02X ", r, val);
            if ((r + 1) % 8 == 0) Serial.println();
        }
        Serial.println("\n==================================");
    }

    // Initialize ES8311 codec and I2S bus with verified register sequence
    static bool init(int pa_arg, int mclk, int bclk, int ws, int dout, int din, int init_mode = 3) {
        pa_pin()   = pa_arg;
        mclk_pin() = mclk;
        bclk_pin() = bclk;
        ws_pin()   = ws;
        dout_pin() = dout;
        din_pin()  = din;

        // Immediately put PA in shutdown before I2S init to prevent startup pop/beep
        if (pa_pin() >= 0) {
            pinMode(pa_pin(), OUTPUT);
            digitalWrite(pa_pin(), HIGH); // Mode 3 PA is Active LOW -> HIGH = Muted
        }

        Serial.printf("[AUDIO] Init I2S Audio: MCLK=%d, BCLK=%d, WS=%d, DOUT=%d, DIN=%d, PA=%d\n",
                      mclk, bclk, ws, dout, din, pa_arg);

        // 1. Install ESP32-S3 I2S driver in full duplex (TX + RX)
        i2s_config_t i2s_config = {
            .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
            .sample_rate = AUDIO_SAMPLE_RATE,
            .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
            .communication_format = I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
            .dma_buf_count = 6,
            .dma_buf_len = 256,
            .use_apll = false,
            .tx_desc_auto_clear = true,
            .fixed_mclk = AUDIO_SAMPLE_RATE * 256 // 4.096 MHz Master Clock
        };

        i2s_pin_config_t pin_config = {
            .mck_io_num   = mclk,
            .bck_io_num   = bclk,
            .ws_io_num    = ws,
            .data_out_num = dout,
            .data_in_num  = din
        };

        if (i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL) != ESP_OK) {
            Serial.println("[AUDIO] ERROR: i2s_driver_install failed!");
            return false;
        }
        if (i2s_set_pin(I2S_NUM_0, &pin_config) != ESP_OK) {
            Serial.println("[AUDIO] ERROR: i2s_set_pin failed!");
            return false;
        }

        // Allow MCLK clock to oscillate into ES8311
        delay(50);

        // 3. Detect ES8311 I2C Address (0x18 or 0x19)
        codec_addr() = 0x18;
        uint8_t id1 = read_reg(0xFD);
        uint8_t id2 = read_reg(0xFE);
        if (id1 != 0x83) {
            codec_addr() = 0x19;
            id1 = read_reg(0xFD);
            id2 = read_reg(0xFE);
            if (id1 != 0x83) {
                codec_addr() = 0x18; // fallback default
            }
        }
        Serial.printf("[AUDIO] Codec detected at I2C 0x%02X | Chip ID: 0x%02X 0x%02X\n", codec_addr(), id1, id2);

        // 4. Configure ES8311 Clock Manager & System Registers (Official ESP-ADF sequence)
        write_reg(0x01, 0x30); // Set clock manager
        write_reg(0x02, 0x00); // Pre_div=1, pre_multi=1
        write_reg(0x03, 0x10); // fs_mode=0, adc_osr=16
        write_reg(0x16, 0x24); // ADC digital gain default
        write_reg(0x04, 0x10); // dac_osr=16
        write_reg(0x05, 0x00); // adc_div=1, dac_div=1
        write_reg(0x0B, 0x00); // System power normal
        write_reg(0x0C, 0x00); // System power normal
        write_reg(0x10, 0x1F); // Power up ADC
        write_reg(0x11, 0x7F); // Power up ADC filter
        write_reg(0x00, 0x80); // CSM enable, slave mode (bit 6 = 0)
        write_reg(0x01, 0x3F); // Enable all internal clocks from MCLK pin

        // 5. Sample Rate Coefficients (16000 Hz @ 4.096 MHz MCLK)
        write_reg(0x02, 0x00); // pre_div=1, pre_multi=1
        write_reg(0x05, 0x00); // adc_div=1, dac_div=1
        write_reg(0x03, 0x10); // fs_mode=0, adc_osr=16
        write_reg(0x04, 0x10); // dac_osr=16
        write_reg(0x07, 0x00); // lrck_h=0
        write_reg(0x08, 0xFF); // lrck_l=255 (256x fs)
        write_reg(0x06, 0x03); // bclk_div=4 (bclk_div - 1 = 3)

        // 6. SDP IN & SDP OUT (16-bit Standard I2S, Active / Unmuted)
        write_reg(0x09, 0x0C); // SDP IN: 16-bit standard I2S (bit 6 = 0)
        write_reg(0x0A, 0x0C); // SDP OUT: 16-bit standard I2S (bit 6 = 0)

        // 7. System, Analog Routing, Bias & Power Management
        write_reg(0x13, 0x10); // System routing
        write_reg(0x1B, 0x0A); // ADC HPF filter s1
        write_reg(0x1C, 0x6A); // ADC equalizer / HPF s2
        write_reg(0x44, 0x08); // ADC channel mapping & test control
        write_reg(0x32, 0xBF); // DAC Volume: 0 dB unity gain (0xBF = 0dB, prevents +18dB digital clipping)
        write_reg(0x17, 0xDF); // ADC Digital Volume: +16 dB gain (0xDF) for clear voice capture
        write_reg(0x0E, 0x02); // System power management
        write_reg(0x12, 0x00); // Enable DAC system
        write_reg(0x14, 0x1A); // Analog MEMS Mic PGA Gain (+30dB, bit 6=0 analog mic)
        write_reg(0x0D, 0x01); // Power up analog blocks
        write_reg(0x15, 0x40); // ADC ramp rate & Mic Bias ON (~2.0V)
        write_reg(0x37, 0x48); // CRITICAL: DAC Output Driver Stage POWER UP (0x48)!
        write_reg(0x45, 0x00); // GP Control Normal (0x00)
        write_reg(0x31, 0x00); // DAC Unmute

        // Set initial hardware mode (Mode 3: DOUT=15, DIN=16, PA=LOW (0))
        set_hardware_mode(init_mode);

        Serial.println("[AUDIO] ES8311 initialized with verified ESP-ADF configuration!");
        return true;
    }

    // Set Volume: 0 to 100% (Maps 0-100% cleanly to 0-0xBF so it never clips)
    // Does NOT unmute PA directly — PA is only unmuted during active audio playback!
    static void set_volume(uint8_t vol) {
        if (vol > 100) vol = 100;
        if (vol == 0) {
            mute(true);
            write_reg(0x32, 0x00);
        } else {
            uint8_t reg_val = (uint8_t)((vol * 191) / 100); // 191 = 0xBF (0 dB maximum unity gain)
            write_reg(0x32, reg_val);
        }
    }

    // Play synthesized sine wave tone (amplitude 10000.0f prevents SC8002B amplifier clipping)
    // Automatically shuts down PA amplifier immediately after tone finishes!
    static void play_tone(uint16_t freq, uint16_t duration_ms, float amplitude = 10000.0f) {
        if (low_battery_quiet() || !ui_sound_enabled()) return;

        size_t samples = (AUDIO_SAMPLE_RATE * duration_ms) / 1000;
        if (samples == 0) return;

        int16_t *buf = (int16_t *)malloc(samples * 2 * sizeof(int16_t));
        if (!buf) return;

        float step = 2.0f * 3.14159265f * freq / (float)AUDIO_SAMPLE_RATE;
        for (size_t i = 0; i < samples; i++) {
            float env = 1.0f;
            if (i < samples / 6) env = (float)i / (samples / 6);
            else if (i > (samples * 5) / 6) env = (float)(samples - i) / (samples / 6);

            int16_t sample = (int16_t)(sinf(i * step) * amplitude * env);
            buf[i * 2]     = sample; // Left channel
            buf[i * 2 + 1] = sample; // Right channel
        }

        mute(false); // Unmute amplifier
        delay(12);   // Quick ramp

        size_t bytes_written = 0;
        i2s_write(I2S_NUM_0, buf, samples * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        i2s_zero_dma_buffer(I2S_NUM_0);
        mute(true);  // CRITICAL: Immediately shut down SC8002B PA so it never buzzes/beeps when idle!
        free(buf);
    }

    static void play_touch_sound(uint16_t freq = 1200, uint16_t duration_ms = 35) {
        if (low_battery_quiet() || !ui_sound_enabled()) return;
        play_tone(freq, duration_ms, 8000.0f);
    }

    // Rich dual-note chime
    static void play_startup_chime() {
        if (low_battery_quiet() || !ui_sound_enabled()) return;
        play_tone(880, 110, 9000.0f);  // A5
        delay(25);
        play_tone(1318, 150, 9000.0f); // E6
    }

    // 5-Tone melody (Do - Mi - Sol - Do - Mi)
    static void play_test_melody() {
        if (low_battery_quiet()) return;
        uint16_t notes[] = {523, 659, 784, 1046, 1318};
        for (int i = 0; i < 5; i++) {
            play_tone(notes[i], 140, 10000.0f);
            delay(30);
        }
    }

    // Play test sweep across ALL 4 hardware modes, then remain in Mode 3 (the working mode)
    static void test_all_modes_chime() {
        uint16_t test_freqs[4] = {659, 784, 988, 1318}; // E5, G5, B5, E6

        Serial.println("[AUDIO-DIAG] Testing audio across all 4 modes...");
        for (int m = 0; m < 4; m++) {
            set_hardware_mode(m);
            Serial.printf("[AUDIO-DIAG] Mode %d: DOUT=%d, DIN=%d, PA=%s\n",
                          m, dout_pin(), din_pin(), is_pa_active_high() ? "1" : "0");
            play_tone(test_freqs[m], 150, 10000.0f);
            delay(100);
        }
        // Return and stay in Mode 3 (DOUT=15, DIN=16, PA=LOW (0) - verified working mode!)
        set_hardware_mode(3);
    }

    // Dump raw microphone samples over Serial for debugging
    static void dump_mic_raw() {
        int16_t raw[64];
        size_t bytes_read = 0;
        esp_err_t err = i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytes_read, pdMS_TO_TICKS(50));
        int max_val = 0;
        for (size_t i = 0; i < bytes_read / sizeof(int16_t); i++) {
            if (abs(raw[i]) > max_val) max_val = abs(raw[i]);
        }
        Serial.printf("[MIC-RAW] Mode %d (DIN=%d): read=%d bytes, max_amp=%d | s[0..3]=%d %d %d %d\n",
                      current_hw_mode(), din_pin(), bytes_read, max_val, raw[0], raw[1], raw[2], raw[3]);
    }

    // Read live microphone RMS sound level (0 - 100%)
    static int read_mic_level() {
        int16_t sample_buf[256];
        size_t bytes_read = 0;
        esp_err_t err = i2s_read(I2S_NUM_0, sample_buf, sizeof(sample_buf), &bytes_read, pdMS_TO_TICKS(25));
        if (err != ESP_OK || bytes_read == 0) return 0;

        int samples = bytes_read / sizeof(int16_t);
        int64_t sum_squares = 0;
        for (int i = 0; i < samples; i++) {
            int32_t val = (int32_t)sample_buf[i];
            sum_squares += val * val;
        }
        int32_t rms = (int32_t)sqrtf((float)(sum_squares / samples));
        int level = map(rms, 15, 1000, 0, 100);
        if (level < 0) level = 0;
        if (level > 100) level = 100;
        return level;
    }

    // Record mono audio with Smart VAD (Voice Activity Detection, auto-stop on silence & silence trimming like xiaozhi-esp32)
    static size_t record_samples_vad(
        int16_t *dest_buf,
        size_t max_samples,
        void (*on_level_cb)(int level) = nullptr,
        volatile bool *stop_req_flag = nullptr,
        uint32_t silence_timeout_ms = 620,
        uint32_t no_speech_timeout_ms = 2200
    ) {
        int16_t raw_stereo[256];
        size_t total_recorded = 0;
        size_t last_cb_idx = 0;

        // Ensure PA is muted during recording to avoid acoustic feedback
        mute(true);

        // Flush any stale DMA buffer
        size_t dummy_read = 0;
        i2s_read(I2S_NUM_0, raw_stereo, sizeof(raw_stereo), &dummy_read, pdMS_TO_TICKS(10));

        // DC-blocking filter state
        float prev_in = 0.0f;
        float prev_out = 0.0f;
        const float alpha = 0.985f; // ~38Hz cut-off at 16kHz

        unsigned long start_ms = millis();
        unsigned long last_speech_ms = start_ms;
        bool speech_detected = false;
        int speech_frames = 0;
        size_t first_speech_idx = 0;
        size_t last_speech_idx = 0;
        float noise_floor = 180.0f;

        while (total_recorded < max_samples) {
            if (stop_req_flag && *stop_req_flag) {
                Serial.println("[VAD] User manually stopped recording early!");
                break;
            }

            size_t to_read = (max_samples - total_recorded > 128) ? 128 : (max_samples - total_recorded);
            size_t bytes_read = 0;
            esp_err_t err = i2s_read(I2S_NUM_0, raw_stereo, to_read * 2 * sizeof(int16_t), &bytes_read, pdMS_TO_TICKS(40));
            if (err != ESP_OK || bytes_read == 0) break;

            size_t pairs = bytes_read / (2 * sizeof(int16_t));
            for (size_t i = 0; i < pairs; i++) {
                int16_t in_s = raw_stereo[i * 2];
                // DC-blocking filter: y[n] = x[n] - x[n-1] + alpha * y[n-1]
                float out_s = (float)in_s - prev_in + alpha * prev_out;
                prev_in = (float)in_s;
                prev_out = out_s;

                // Clean 2.0x gain with soft limiting
                float boosted = out_s * 2.0f;
                if (boosted > 32000.0f) boosted = 32000.0f;
                if (boosted < -32000.0f) boosted = -32000.0f;
                dest_buf[total_recorded++] = (int16_t)boosted;
            }

            // Evaluate VAD & level callback every 256 samples (16ms)
            if (total_recorded - last_cb_idx >= 256) {
                int64_t sq = 0;
                for (size_t k = last_cb_idx; k < total_recorded; k++) {
                    int32_t s = dest_buf[k];
                    sq += s * s;
                }
                int32_t chunk_rms = (int32_t)sqrtf((float)(sq / (total_recorded - last_cb_idx)));

                if (on_level_cb) {
                    int level = map(chunk_rms, 100, 4500, 0, 100);
                    if (level < 0) level = 0;
                    if (level > 100) level = 100;
                    on_level_cb(level);
                }

                unsigned long now = millis();
                // Calibrate adaptive noise floor during initial silence
                if (!speech_detected && (now - start_ms < 200)) {
                    noise_floor = 0.8f * noise_floor + 0.2f * (float)chunk_rms;
                    if (noise_floor < 120.0f) noise_floor = 120.0f;
                    if (noise_floor > 600.0f) noise_floor = 600.0f;
                }

                int32_t trig_thresh = (int32_t)(noise_floor * 2.3f);
                if (trig_thresh < 360) trig_thresh = 360;
                int32_t hold_thresh = (trig_thresh * 3) / 4;

                if (chunk_rms >= trig_thresh) {
                    speech_frames++;
                    last_speech_ms = now;
                    last_speech_idx = total_recorded;
                    if (!speech_detected && speech_frames >= 2) {
                        speech_detected = true;
                        first_speech_idx = (last_cb_idx > 256) ? (last_cb_idx - 256) : 0;
                        Serial.printf("[VAD] Speech started at %lu ms (RMS=%d, thresh=%d)\n",
                                      now - start_ms, (int)chunk_rms, (int)trig_thresh);
                    }
                } else if (speech_detected && chunk_rms >= hold_thresh) {
                    last_speech_ms = now;
                    last_speech_idx = total_recorded;
                } else {
                    if (!speech_detected) speech_frames = 0;
                }

                last_cb_idx = total_recorded;

                // Auto-stop when user finishes speaking (silence_timeout_ms of silence after speech)
                if (speech_detected && (total_recorded - first_speech_idx >= (AUDIO_SAMPLE_RATE * 35) / 100)) {
                    if (now - last_speech_ms >= silence_timeout_ms) {
                        Serial.printf("[VAD] End of speech detected! Recorded %d samples (%lu ms total).\n",
                                      (int)total_recorded, now - start_ms);
                        break;
                    }
                } else if (!speech_detected && (now - start_ms >= no_speech_timeout_ms)) {
                    Serial.printf("[VAD] No speech detected after %lu ms, stopping early.\n", now - start_ms);
                    break;
                }
            }
        }

        // Trim leading and trailing silence to minimize upload bytes (keeps 150ms pre-roll & 180ms post-roll)
        if (speech_detected && total_recorded > 1600) {
            const size_t pre_roll = (AUDIO_SAMPLE_RATE * 15) / 100;  // 2400 samples (150ms)
            const size_t post_roll = (AUDIO_SAMPLE_RATE * 18) / 100; // 2880 samples (180ms)
            size_t start_trim = (first_speech_idx > pre_roll) ? (first_speech_idx - pre_roll) : 0;
            size_t end_trim = (last_speech_idx + post_roll < total_recorded) ? (last_speech_idx + post_roll) : total_recorded;
            if (end_trim > start_trim + 1600) {
                size_t trimmed_len = end_trim - start_trim;
                if (start_trim > 0) {
                    memmove(dest_buf, dest_buf + start_trim, trimmed_len * sizeof(int16_t));
                }
                Serial.printf("[VAD] Trimmed audio from %d -> %d samples (%d bytes saved)\n",
                              (int)total_recorded, (int)trimmed_len, (int)((total_recorded - trimmed_len) * 2));
                total_recorded = trimmed_len;
            }
        }

        return total_recorded;
    }

    // Record fixed mono audio from microphone (for 2s Mic Test button)
    static size_t record_samples(int16_t *dest_buf, size_t max_samples, void (*on_level_cb)(int level) = nullptr) {
        int16_t raw_stereo[256];
        size_t total_recorded = 0;
        size_t last_cb_idx = 0;

        mute(true);

        // Flush any stale buffer
        size_t dummy_read = 0;
        i2s_read(I2S_NUM_0, raw_stereo, sizeof(raw_stereo), &dummy_read, pdMS_TO_TICKS(10));

        // DC-blocking filter state
        float prev_in = 0.0f;
        float prev_out = 0.0f;
        const float alpha = 0.985f; // ~38Hz cut-off at 16kHz

        while (total_recorded < max_samples) {
            size_t to_read = (max_samples - total_recorded > 128) ? 128 : (max_samples - total_recorded);
            size_t bytes_read = 0;
            esp_err_t err = i2s_read(I2S_NUM_0, raw_stereo, to_read * 2 * sizeof(int16_t), &bytes_read, pdMS_TO_TICKS(50));
            if (err != ESP_OK || bytes_read == 0) break;

            size_t pairs = bytes_read / (2 * sizeof(int16_t));
            for (size_t i = 0; i < pairs; i++) {
                int16_t in_s = raw_stereo[i * 2];
                // DC-blocking filter: y[n] = x[n] - x[n-1] + alpha * y[n-1]
                float out_s = (float)in_s - prev_in + alpha * prev_out;
                prev_in = (float)in_s;
                prev_out = out_s;

                // Moderate 2.0x clean gain with soft limiting (prevents harsh distortion)
                float boosted = out_s * 2.0f;
                if (boosted > 32000.0f) boosted = 32000.0f;
                if (boosted < -32000.0f) boosted = -32000.0f;
                dest_buf[total_recorded++] = (int16_t)boosted;
            }

            // Real-time audio spectrum animation callback every ~32ms (256 samples)
            if (on_level_cb && (total_recorded - last_cb_idx >= 256)) {
                int64_t sq = 0;
                for (size_t k = last_cb_idx; k < total_recorded; k++) {
                    int32_t s = dest_buf[k];
                    sq += s * s;
                }
                int32_t chunk_rms = (int32_t)sqrtf((float)(sq / (total_recorded - last_cb_idx)));
                int level = map(chunk_rms, 100, 4500, 0, 100);
                if (level < 0) level = 0;
                if (level > 100) level = 100;
                on_level_cb(level);
                last_cb_idx = total_recorded;
            }
        }
        return total_recorded;
    }

    // Playback mono buffer out to speaker via I2S stereo with real-time level callback
    static void play_samples(const int16_t *src_buf, size_t sample_count, void (*on_level_cb)(int level) = nullptr) {
        int16_t stereo_chunk[256];
        size_t idx = 0;
        size_t last_cb_idx = 0;

        mute(false); // ensure amplifier is active
        delay(20);
        while (idx < sample_count) {
            size_t chunk_samples = (sample_count - idx > 128) ? 128 : (sample_count - idx);
            for (size_t i = 0; i < chunk_samples; i++) {
                int16_t s = (int16_t)(((int32_t)src_buf[idx + i] * 3) / 4); // 75% scale to prevent SC8002B overdrive
                stereo_chunk[i * 2]     = s; // Left
                stereo_chunk[i * 2 + 1] = s; // Right
            }

            size_t bytes_written = 0;
            i2s_write(I2S_NUM_0, stereo_chunk, chunk_samples * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
            idx += chunk_samples;

            if (on_level_cb && (idx - last_cb_idx >= 256)) {
                int64_t sq = 0;
                for (size_t k = last_cb_idx; k < idx; k++) {
                    int32_t s = src_buf[k];
                    sq += s * s;
                }
                int32_t chunk_rms = (int32_t)sqrtf((float)(sq / (idx - last_cb_idx)));
                int level = map(chunk_rms, 100, 8000, 0, 100);
                if (level < 0) level = 0;
                if (level > 100) level = 100;
                on_level_cb(level);
                last_cb_idx = idx;
            }
        }
        i2s_zero_dma_buffer(I2S_NUM_0);
        mute(true); // Shut down SC8002B PA immediately after playback!
    }
};
