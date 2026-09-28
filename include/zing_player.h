#pragma once
#include <Arduino.h>

namespace ZingPlayer {

struct SongInfo {
    char title[128];
    char artist[128];
    char song_id[32];
    int duration;
    bool found;
};

void init();
bool is_playing();
void stop();

// Tìm kiếm thông tin bài hát từ Zing MP3 Proxy
bool search_song(const char *query, SongInfo &out_info);

// Stream trực tiếp và phát bài hát qua chip DAC ES8311 & Loa I2S
// on_level_cb: callback cập nhật mức âm lượng/phổ âm thanh
// status_cb: callback cập nhật trạng thái hiển thị trên giao diện LVGL
bool play_song_by_query(const char *query,
                        void (*on_level_cb)(int level) = nullptr,
                        void (*status_cb)(const char *msg) = nullptr);

} // namespace ZingPlayer
