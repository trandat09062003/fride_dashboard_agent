#pragma once
#include <Arduino.h>

namespace GoogleTTS {
    // Speak a Vietnamese text string via Google Translate TTS over WiFi through ES8311 I2S speaker
    // Returns true on success, false on network/decode failure
    bool speak(const char *text_utf8, void (*on_level_cb)(int level) = nullptr);
}
