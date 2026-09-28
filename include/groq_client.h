#pragma once
#include <Arduino.h>

namespace GroqAI {

// Initialize Groq AI Client
void init();

// Pre-warm TLS connection to api.groq.com:443 asynchronously in background during voice recording
void prewarm_connection_async();

// Convert recorded 16kHz 16-bit PCM voice samples to text via Groq Whisper API
// Returns true if transcription succeeded, false otherwise.
bool speech_to_text(const int16_t *samples, size_t sample_count, char *out_text, size_t max_out_len);

// Ask Groq LLM (llama-3.3-70b-versatile / llama-3.1-8b-instant) with context of fridge temperature
// Returns true if completion succeeded, false otherwise.
bool chat_completion(const char *user_prompt, char *out_answer, size_t max_out_len, int temp_mat, int temp_dong, const char *time_context = nullptr);

} // namespace GroqAI
