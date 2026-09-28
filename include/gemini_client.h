#pragma once
#include <Arduino.h>

namespace GeminiAI {

// Initialize Gemini AI Client
void init();

// Generate content via Google Gemini REST API
// Returns true if completion succeeded, false otherwise.
bool chat_completion(const char *user_prompt, char *out_answer, size_t max_out_len, int temp_mat, int temp_dong);

} // namespace GeminiAI
