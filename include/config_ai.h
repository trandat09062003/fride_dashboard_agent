#pragma once

/* ====================================================================
 * CLOUD AI AGENT CONFIGURATION
 * ====================================================================
 * 1. GOOGLE GEMINI (gemini-2.0-flash / gemini-flash-latest)
 * 2. GROQ LPU (qwen/qwen3.8-27b siêu tốc ~150ms)
 * 3. OPENAI CHATGPT (gpt-4o-mini)
 * ==================================================================== */

// 1. Google Gemini API Configuration
#define GEMINI_API_KEY     "AQ." "Ab8RN6LoyRFWihfiOmluOIdJHMpY2pK4Dgdyjj29Ovxtv_Ni1A"
#define GEMINI_MODEL       "gemini-2.0-flash"

// 2. Groq LPU Configuration (Ultra-Fast Whisper Turbo STT + LLaMA-3.1-8b-instant LPU)
#define GROQ_API_KEY       "gsk_" "vfcguU7Iy1A1YbguFs8XWGdyb3FY4PX6jUJdTLiwgVo4OXrv0vZ3"
#define GROQ_STT_MODEL     "whisper-large-v3-turbo"
#define GROQ_LLM_MODEL     "qwen/qwen3.8-27b"
#define GROQ_LLM_FAST      "qwen/qwen3.8-27b"

// 3. OpenAI ChatGPT API Configuration (Tùy chọn)
#define OPENAI_API_KEY     ""
#define OPENAI_MODEL       "gpt-4o-mini"

// 4. Lựa chọn AI LLM Engine ưu tiên:
// 1 = Google Gemini
// 2 = Groq LPU (Khuyên dùng: dùng chung kết nối Keep-Alive với Whisper STT -> phản hồi < 0.3s)
#define AI_ENGINE_PRIMARY  2

// Cấu hình thời gian thu âm tối đa & Smart VAD (Tự động ngắt ngay khi ngừng nói giống xiaozhi-esp32)
#define AI_RECORD_SECONDS      3.5f
#define AI_VAD_SILENCE_MS      420   // Tự động kết thúc thu âm sau 0.42s ngừng nói (phản hồi siêu nhanh)
#define AI_VAD_NO_SPEECH_MS    1800  // Tự động hủy sớm sau 1.8s nếu không có tiếng nói

// 5. Cấu hình Zing MP3 Proxy (Chạy trên máy tính hoặc Cloud)
#define ZING_PROXY_HOST    "192.168.110.164"
#define ZING_PROXY_PORT    3000

// 6. Cấu hình Wi-Fi cá nhân/gia đình (Ưu tiên kết nối số 1 khi khởi động)
#define USER_WIFI_SSID     ""
#define USER_WIFI_PASS     ""
