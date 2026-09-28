#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <time.h>
#include "esp_sntp.h"
#include <Preferences.h>
#include <driver/i2c.h>
#include <lvgl.h>
#include "Arduino_ST77922_Bus.h"
#include "es8311_driver.h"
#include "google_tts.h"
#include "groq_client.h"
#include "gemini_client.h"
#include "config_ai.h"
#include "vn_strip.h"
#include "zing_player.h"
#include "fridge_images.h"
#include "fridge_icons.h"
#include "license_guard.h"

/* ====================================================================
 * HARDWARE PINS (LCDWiki ES3C35P / DIYMORE ESP32-S3 3.5" QSPI)
 * ==================================================================== */
#define LCD_CS      10
#define LCD_SCK     12
#define LCD_D0      11
#define LCD_D1      13
#define LCD_D2      14
#define LCD_D3      9
#define LCD_BL      41  // Backlight PWM

// TOUCH ST77922 TDDI (I2C @ 0x55)
#define TOUCH_SDA   38
#define TOUCH_SCL   39
#define TOUCH_RST   48  // GPIO 48: Hardware Reset (PR #2112)
#define TOUCH_INT   47  // GPIO 47: Interrupt

// AUDIO HARDWARE PINS (CONFIRMED BY SCHEMATIC & DATASHEET)
#define SPEAKER_PA_EN 1   // GPIO 1: Connected to SHUTDOWN (Pin 1) of SC8002B/FM8002E (Active LOW)
#define I2S_MCLK      17  // GPIO 17: MCLK Master Clock
#define I2S_BCLK      18  // GPIO 18: BCLK Bit Clock
#define I2S_WS        21  // GPIO 21: WS / LRCK Word Select
#define I2S_DOUT      15  // GPIO 15: Pin 21 of ESP32-S3 (Net I2S_DI -> ES8311 DSDIN -> Speaker DAC)
#define I2S_DIN       16  // GPIO 16: Pin 22 of ESP32-S3 (Net I2S_DO <- ES8311 ASDOUT <- Mic ADC)

#define BAT_ADC_PIN   8   // GPIO 8: BAT_ADC (R14=100K / R15=100K 1:2 voltage divider on Schematic)

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 480

// WiFi & Preferences
static String cur_wifi_ssid = "";
static String cur_wifi_pass = "";
static Preferences wifi_prefs;
static lv_obj_t *lbl_rw_wifi = nullptr;
static lv_obj_t *lbl_time1 = nullptr;
static lv_obj_t *lbl_time2 = nullptr;
static lv_obj_t *lbl_time3 = nullptr;
static lv_obj_t *lbl_time4 = nullptr;

// Battery Monitoring & Low-Battery Silent Protection (Fixes continuous beeping on low battery)
static int s_battery_pct = 100;
static int s_battery_mv = 4150;
static float s_battery_mv_ema = 0.0f;
static bool s_low_bat_silent_mode = false; // Loa luôn hoạt động đầy đủ
static lv_obj_t *lbl_bat_val = nullptr;

static void read_battery_voltage() {
    int raw_mv = analogReadMilliVolts(BAT_ADC_PIN);
    int measured_mv = raw_mv * 2;
    if (measured_mv < 2500) {
        measured_mv = 4150; // USB powered or battery divider floating
    }
    if (s_battery_mv_ema <= 100.0f) {
        s_battery_mv_ema = (float)measured_mv;
    } else {
        s_battery_mv_ema = s_battery_mv_ema * 0.85f + (float)measured_mv * 0.15f;
    }
    s_battery_mv = (int)(s_battery_mv_ema + 0.5f);
    int pct = (s_battery_mv - 3400) * 100 / (4200 - 3400);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_battery_pct = pct;

    // Chỉ ngắt âm báo nếu pin thực sự cạn kiệt (< 3.1V) để bảo vệ mạch
    if (s_battery_mv < 3100) {
        ES8311_Audio::set_low_battery_quiet(true);
    } else {
        ES8311_Audio::set_low_battery_quiet(false);
    }

    if (lbl_bat_val) {
        lv_label_set_text_fmt(lbl_bat_val, "%d%% (%d.%02dV)", s_battery_pct, s_battery_mv / 1000, (s_battery_mv % 1000) / 10);
        if (s_battery_pct > 35) {
            lv_obj_set_style_text_color(lbl_bat_val, lv_color_hex(0x22C55E), 0);
        } else if (s_battery_pct > 15) {
            lv_obj_set_style_text_color(lbl_bat_val, lv_color_hex(0xF59E0B), 0);
        } else {
            lv_obj_set_style_text_color(lbl_bat_val, lv_color_hex(0xEF4444), 0);
        }
    }
}

static lv_obj_t *lbl_bat_sub = nullptr;
static lv_obj_t *bar_bat_pct = nullptr;
static lv_obj_t *sw_low_bat_mute = nullptr;

static lv_obj_t *footer_img[4] = {nullptr};
static lv_obj_t *footer_lbl[4] = {nullptr};

static bool s_eco_mode = true;
static bool s_child_lock = false;
static bool s_fast_freeze = false;
static bool s_fast_cool = false;
static int s_cooling_mode = 0; // 0: Làm lạnh nhanh, 1: Chế độ Eco, 2: Tùy chỉnh
static lv_obj_t *lbl_home_eco_state = nullptr;
static lv_obj_t *lbl_home_lock_state = nullptr;
static lv_obj_t *sw_settings_noti = nullptr;
static lv_obj_t *sw_settings_lock = nullptr;
static lv_obj_t *sw_settings_eco = nullptr;
static lv_obj_t *lbl_bright_pct = nullptr;
static lv_obj_t *lbl_vol_pct = nullptr;
static lv_obj_t *card_modes[3] = {nullptr};
static lv_obj_t *lbl_tab2_mat = nullptr;

// Screen Timeout Management
static int s_screen_timeout_sec = 60; // 30s, 60s, 120s, 300s, 0 (Never)
static const int s_timeout_options[] = {30, 60, 120, 300, 0};
static const char *s_timeout_labels[] = {"30 giây", "1 phút", "2 phút", "5 phút", "Không tắt"};
static int s_timeout_idx = 1; // Default 1 min (60s)
static uint32_t s_last_activity_time = 0;
static bool s_screen_sleeping = false;
static lv_obj_t *lbl_timeout_val = nullptr;

struct ScannedWiFi {
    char ssid[34];
    int rssi;
    bool is_open;
};
static ScannedWiFi s_scanned_wifis[30];
static int s_scanned_count = 0;
static char s_selected_ssid[34] = {0};

struct KnownWiFi {
    const char *ssid;
    const char *pass;
};
static const KnownWiFi s_known_wifis[] = {
    {USER_WIFI_SSID, USER_WIFI_PASS},
    {"VIETSET_TECH", "vs68686868"}
};

static const char* get_known_wifi_password(const char *ssid) {
    if (!ssid || strlen(ssid) == 0) return nullptr;
    for (size_t i = 0; i < sizeof(s_known_wifis) / sizeof(s_known_wifis[0]); i++) {
        if (strcmp(ssid, s_known_wifis[i].ssid) == 0) {
            return s_known_wifis[i].pass;
        }
    }
    return nullptr;
}

#define MAX_SAVED_WIFIS 5
static String s_nvs_saved_ssid = "";
static String s_nvs_saved_pass = "";
static String s_nvs_hist_ssid[MAX_SAVED_WIFIS];
static String s_nvs_hist_pass[MAX_SAVED_WIFIS];

static void reload_nvs_wifi_cache() {
    wifi_prefs.begin("wifi_cfg", true);
    s_nvs_saved_ssid = wifi_prefs.getString("ssid", "");
    s_nvs_saved_pass = wifi_prefs.getString("pass", "");
    for (int i = 0; i < MAX_SAVED_WIFIS; i++) {
        char ks[12], kp[12];
        snprintf(ks, sizeof(ks), "ssid_%d", i);
        snprintf(kp, sizeof(kp), "pass_%d", i);
        s_nvs_hist_ssid[i] = wifi_prefs.getString(ks, "");
        s_nvs_hist_pass[i] = wifi_prefs.getString(kp, "");
    }
    wifi_prefs.end();
}

static void save_wifi_credentials_to_nvs(const char *ssid, const char *pass) {
    if (!ssid || strlen(ssid) == 0) return;
    reload_nvs_wifi_cache();

    s_nvs_saved_ssid = String(ssid);
    s_nvs_saved_pass = String(pass ? pass : "");

    // Update MRU history list in NVS so multiple networks never require re-login
    int existing_idx = -1;
    for (int i = 0; i < MAX_SAVED_WIFIS; i++) {
        if (s_nvs_hist_ssid[i].equals(ssid)) {
            existing_idx = i;
            break;
        }
    }
    int shift_from = (existing_idx >= 0) ? existing_idx : (MAX_SAVED_WIFIS - 1);
    for (int i = shift_from; i > 0; i--) {
        s_nvs_hist_ssid[i] = s_nvs_hist_ssid[i - 1];
        s_nvs_hist_pass[i] = s_nvs_hist_pass[i - 1];
    }
    s_nvs_hist_ssid[0] = s_nvs_saved_ssid;
    s_nvs_hist_pass[0] = s_nvs_saved_pass;

    wifi_prefs.begin("wifi_cfg", false);
    wifi_prefs.putString("ssid", s_nvs_saved_ssid);
    wifi_prefs.putString("pass", s_nvs_saved_pass);
    for (int i = 0; i < MAX_SAVED_WIFIS; i++) {
        char ks[12], kp[12];
        snprintf(ks, sizeof(ks), "ssid_%d", i);
        snprintf(kp, sizeof(kp), "pass_%d", i);
        wifi_prefs.putString(ks, s_nvs_hist_ssid[i]);
        wifi_prefs.putString(kp, s_nvs_hist_pass[i]);
    }
    wifi_prefs.end();
}

static bool is_saved_or_known_wifi(const char *ssid, String &out_pass) {
    if (!ssid || strlen(ssid) == 0) return false;
    if (s_nvs_saved_ssid.length() > 0 && strcmp(ssid, s_nvs_saved_ssid.c_str()) == 0) {
        out_pass = s_nvs_saved_pass;
        return true;
    }
    for (int i = 0; i < MAX_SAVED_WIFIS; i++) {
        if (s_nvs_hist_ssid[i].length() > 0 && strcmp(ssid, s_nvs_hist_ssid[i].c_str()) == 0) {
            out_pass = s_nvs_hist_pass[i];
            return true;
        }
    }
    const char *p = get_known_wifi_password(ssid);
    if (p) {
        out_pass = String(p);
        return true;
    }
    return false;
}

static lv_timer_t *s_wifi_conn_timer = nullptr;
static int s_wifi_conn_ticks = 0;
static char s_connecting_ssid[34] = {0};
static char s_connecting_pass[64] = {0};

static lv_obj_t *wifi_full_screen = nullptr;
static lv_obj_t *wifi_current_card = nullptr;
static lv_obj_t *lbl_wifi_curr_title = nullptr;
static lv_obj_t *lbl_wifi_curr_detail = nullptr;
static lv_obj_t *lbl_wifi_scan_count = nullptr;
static lv_obj_t *wifi_list_cont = nullptr;
static lv_obj_t *btn_wifi_rescan = nullptr;
static lv_obj_t *lbl_wifi_rescan = nullptr;
static lv_obj_t *btn_wifi_pw_connect = nullptr;
static lv_obj_t *lbl_wifi_pw_connect = nullptr;

static lv_obj_t *wifi_pw_sheet = nullptr;
static lv_obj_t *ta_wifi_pw = nullptr;
static lv_obj_t *kb_wifi_pw = nullptr;
static lv_obj_t *lbl_wifi_pw_status = nullptr;
static bool s_pw_visible = true;

/* Display Bus */
static Arduino_ST77922_Bus tft(LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);

/* LVGL Buffers */
static lv_disp_draw_buf_t draw_buf;
#define BUF_LINES 40
static lv_color_t *disp_buf1 = nullptr;
static lv_color_t *disp_buf2 = nullptr;

/* System State */
int temp_mat = 4;           // 0 - 10 C
int temp_dong = -18;        // -24 - -14 C
int screen_brightness = 75; // 10 - 100%
int sound_volume = 65;      // 0 - 100%
static uint8_t touch_max_points = 1;

/* UI Elements */
lv_obj_t *tv = nullptr;
lv_obj_t *lbl_home_mat = nullptr;
lv_obj_t *lbl_home_dong = nullptr;
lv_obj_t *lbl_ctrl_mat = nullptr;
lv_obj_t *lbl_ctrl_dong = nullptr;
static lv_obj_t *lbl_settings_temp = nullptr;
lv_obj_t *slider_mat = nullptr;
lv_obj_t *slider_dong = nullptr;
lv_obj_t *lbl_ai_bubble = nullptr;
static lv_obj_t *box_bubble = nullptr;
lv_obj_t *lbl_ai_status = nullptr;
lv_obj_t *btn_robot_orb = nullptr;
lv_obj_t *lbl_robot_eyes = nullptr;
lv_obj_t *footer_btn[4] = {nullptr};

// Tab 3 AI Assistant & Minimalist Audio Spectrum Elements
#define SPECTRUM_BARS 15
static lv_obj_t *bar_viz[SPECTRUM_BARS] = {nullptr};
static lv_obj_t *bar_vu = nullptr;
static lv_obj_t *lbl_mic_db = nullptr;
static lv_timer_t *mic_viz_timer = nullptr;
static int16_t *voice_test_buffer = nullptr;
static int ai_answer_idx = 0;

/* ====================================================================
 * BACKLIGHT CONTROL
 * ==================================================================== */
void set_backlight(int percent) {
    if (percent <= 0) {
        ledcWrite(0, 0); // Tat hoan toan den nen LCD (Blackout)
        return;
    }
    if (percent < 10) percent = 10;
    if (percent > 100) percent = 100;
    screen_brightness = percent;
    int duty = map(percent, 0, 100, 0, 255);
    ledcWrite(0, duty);
}

/* ====================================================================
 * DISPLAY FLUSH & ROUNDER
 * ==================================================================== */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint16_t x1 = (area->x1 >> 2) << 2;
    uint16_t x2 = ((area->x2 >> 2) << 2) + 3;
    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;
    uint32_t count = (x2 - x1 + 1) * (y2 - y1 + 1);

    uint16_t *p = (uint16_t *)color_p;
    for (uint32_t i = 0; i < count; i++) {
        p[i] = (p[i] << 8) | (p[i] >> 8);
    }

    tft.setAddrWindow(x1, y1, x2, y2);
    tft.writePixels(p, count);
    lv_disp_flush_ready(disp);
}

void my_rounder_cb(lv_disp_drv_t *disp_drv, lv_area_t *area) {
    area->x1 = (area->x1 >> 2) << 2;
    area->x2 = ((area->x2 >> 2) << 2) + 3;
}

/* ====================================================================
 * VERIFIED TOUCH DRIVER (ESP-IDF I2C AT 0x55)
 * ==================================================================== */
static bool touch_i2c_read(uint16_t reg, uint8_t *data, size_t len) {
    uint8_t wbuf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    esp_err_t err = i2c_master_write_read_device(I2C_NUM_0, 0x55, wbuf, 2, data, len, pdMS_TO_TICKS(50));
    return (err == ESP_OK);
}

static void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
    static int16_t last_x = 0, last_y = 0;
    data->point.x = last_x;
    data->point.y = last_y;
    data->state = LV_INDEV_STATE_REL;

    uint8_t info = 0;
    if (touch_i2c_read(0x0010, &info, 1)) {
        if (info & 0x08) {
            uint8_t buf[7 * 5];
            size_t read_len = 7 * touch_max_points;
            if (read_len < 7) read_len = 7;
            if (read_len > sizeof(buf)) read_len = sizeof(buf);
            if (touch_i2c_read(0x0014, buf, read_len)) {
                for (int i = 0; i < touch_max_points; i++) {
                    if (buf[i * 7] & 0x80) {
                        int16_t x = ((buf[i * 7] & 0x3F) << 8) | buf[i * 7 + 1];
                        int16_t y = ((buf[i * 7 + 2] & 0x3F) << 8) | buf[i * 7 + 3];
                        data->point.x = x;
                        data->point.y = y;
                        last_x = x;
                        last_y = y;

                        s_last_activity_time = millis();
                        if (s_screen_sleeping) {
                            s_screen_sleeping = false;
                            set_backlight(screen_brightness);
                            data->state = LV_INDEV_STATE_REL;
                            if (WiFi.status() != WL_CONNECTED && cur_wifi_ssid.length() > 0) {
                                WiFi.reconnect();
                            }
                        } else {
                            data->state = LV_INDEV_STATE_PR;
                        }
                        break;
                    }
                }
            }
        }
    }
}

/* ====================================================================
 * UI SYNCHRONIZATION & INTERACTION
 * ==================================================================== */
void sync_temp_ui() {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", temp_mat);
    if (lbl_home_mat) lv_label_set_text(lbl_home_mat, buf);
    if (lbl_tab2_mat) lv_label_set_text(lbl_tab2_mat, buf); // Đồng bộ nhiệt độ hiện tại Trang 2
    snprintf(buf, sizeof(buf), "%d°C", temp_mat);
    if (lbl_ctrl_mat) lv_label_set_text(lbl_ctrl_mat, buf);
    if (slider_mat && lv_slider_get_value(slider_mat) != temp_mat) {
        lv_slider_set_value(slider_mat, temp_mat, LV_ANIM_OFF);
    }

    snprintf(buf, sizeof(buf), "%d", temp_dong);
    if (lbl_home_dong) lv_label_set_text(lbl_home_dong, buf);
    snprintf(buf, sizeof(buf), "%d°C", temp_dong);
    if (lbl_ctrl_dong) lv_label_set_text(lbl_ctrl_dong, buf);
    if (slider_dong && lv_slider_get_value(slider_dong) != temp_dong) {
        lv_slider_set_value(slider_dong, temp_dong, LV_ANIM_OFF);
    }

    if (lbl_settings_temp) {
        char s_buf[64];
        snprintf(s_buf, sizeof(s_buf), "Mát: %d°C   |   Đông: %d°C   |   " LV_SYMBOL_OK " Tốt", temp_mat, temp_dong);
        lv_label_set_text(lbl_settings_temp, s_buf);
    }
}

void update_nav_highlight(int active_idx) {
    const lv_img_dsc_t *act_icons[] = {&icon_nav_home_act, &icon_nav_fridge_act, &icon_nav_robot_act, &icon_nav_gear_act};
    const lv_img_dsc_t *inact_icons[] = {&icon_nav_home_inact, &icon_nav_fridge_inact, &icon_nav_robot_inact, &icon_nav_gear_inact};

    for (int i = 0; i < 4; i++) {
        if (footer_btn[i]) {
            if (i == active_idx) {
                lv_obj_set_style_bg_color(footer_btn[i], lv_color_hex(0x0C1628), 0);
                lv_obj_set_style_border_side(footer_btn[i], LV_BORDER_SIDE_TOP, 0);
                lv_obj_set_style_border_color(footer_btn[i], lv_color_hex(0x00E5FF), 0);
                lv_obj_set_style_border_width(footer_btn[i], 2, 0);
                if (footer_img[i]) lv_img_set_src(footer_img[i], act_icons[i]);
                if (footer_lbl[i]) lv_obj_set_style_text_color(footer_lbl[i], lv_color_hex(0x00E5FF), 0);
            } else {
                lv_obj_set_style_bg_color(footer_btn[i], lv_color_hex(0x060B18), 0);
                lv_obj_set_style_border_width(footer_btn[i], 0, 0);
                if (footer_img[i]) lv_img_set_src(footer_img[i], inact_icons[i]);
                if (footer_lbl[i]) lv_obj_set_style_text_color(footer_lbl[i], lv_color_hex(0x64748B), 0);
            }
        }
    }
}

static void screen_gesture_cb(lv_event_t *e) {
    if (!tv) return;
    if (wifi_full_screen != nullptr) return; // Never switch tabs when Wi-Fi manager is active!
    // Disable horizontal swipe gesture while on Tab 3 (AI Assistant)
    // to prevent accidental jumping to Tab 1 (Trang chu) when tapping controls!
    if (lv_tabview_get_tab_act(tv) == 2) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    uint16_t curr = lv_tabview_get_tab_act(tv);

    if (dir == LV_DIR_LEFT) {
        if (curr < 3) {
            lv_tabview_set_act(tv, curr + 1, LV_ANIM_ON);
            update_nav_highlight(curr + 1);
        }
    } else if (dir == LV_DIR_RIGHT) {
        if (curr > 0) {
            lv_tabview_set_act(tv, curr - 1, LV_ANIM_ON);
            update_nav_highlight(curr - 1);
        }
    }
}

static void footer_nav_click(lv_event_t *e) {
    int target_tab = (int)(intptr_t)lv_event_get_user_data(e);
    if (tv) {
        lv_tabview_set_act(tv, target_tab, LV_ANIM_ON);
        update_nav_highlight(target_tab);
        ES8311_Audio::play_touch_sound(1200, 30);
    }
}

static void btn_mat_minus_event(lv_event_t *e) {
    if (temp_mat > 0) { temp_mat--; sync_temp_ui(); ES8311_Audio::play_touch_sound(1000, 25); }
}
static void btn_mat_plus_event(lv_event_t *e) {
    if (temp_mat < 10) { temp_mat++; sync_temp_ui(); ES8311_Audio::play_touch_sound(1200, 25); }
}
static void btn_dong_minus_event(lv_event_t *e) {
    if (temp_dong > -24) { temp_dong--; sync_temp_ui(); ES8311_Audio::play_touch_sound(900, 25); }
}
static void btn_dong_plus_event(lv_event_t *e) {
    if (temp_dong < -14) { temp_dong++; sync_temp_ui(); ES8311_Audio::play_touch_sound(1100, 25); }
}
static void slider_mat_event(lv_event_t *e) {
    temp_mat = lv_slider_get_value(slider_mat);
    sync_temp_ui();
}
static void slider_dong_event(lv_event_t *e) {
    temp_dong = lv_slider_get_value(slider_dong);
    sync_temp_ui();
}
static void slider_bright_event(lv_event_t *e) {
    lv_obj_t *s = lv_event_get_target(e);
    int val = lv_slider_get_value(s);
    set_backlight(val);
    if (lbl_bright_pct) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", val);
        lv_label_set_text(lbl_bright_pct, buf);
    }
}
static void slider_vol_event(lv_event_t *e) {
    lv_obj_t *s = lv_event_get_target(e);
    int val = lv_slider_get_value(s);
    sound_volume = val;
    ES8311_Audio::set_volume(sound_volume);
    if (lbl_vol_pct) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", val);
        lv_label_set_text(lbl_vol_pct, buf);
    }
}

static volatile int s_tts_live_level = -1;
static void on_tts_speech_level(int level) {
    s_tts_live_level = level;
}

// Live 15-Bar Equalizer Spectrum & VU-Meter UI Updater
static void update_spectrum_ui(int level) {
    if (lbl_mic_db) {
        lv_label_set_text_fmt(lbl_mic_db, "Mic: %d%%", level);
    }
    if (bar_vu) {
        lv_bar_set_value(bar_vu, level, LV_ANIM_OFF);
    }

    // 15 bars dynamic bell-curve weights
    const int weights[SPECTRUM_BARS] = {25, 40, 55, 70, 85, 95, 100, 95, 85, 70, 55, 40, 30, 25, 20};
    for (int i = 0; i < SPECTRUM_BARS; i++) {
        if (bar_viz[i]) {
            int target_h = 3 + (level * weights[i] * 18) / 10000;
            if (target_h > 20) target_h = 20;
            lv_obj_set_height(bar_viz[i], target_h);

            if (level > 28) {
                lv_obj_set_style_bg_color(bar_viz[i], lv_color_hex(0x00E5FF), 0); // Neon Cyan
            } else if (level > 12) {
                lv_obj_set_style_bg_color(bar_viz[i], lv_color_hex(0x38BDF8), 0); // Sky Blue
            } else {
                lv_obj_set_style_bg_color(bar_viz[i], lv_color_hex(0x0284C7), 0); // Deep Cyan
            }
        }
    }
}

static void clean_voice_text(char *dst, const char *src, size_t max_len) {
    if (!dst || !src || max_len == 0) return;

    // Check and strip reasoning blocks <think>...</think> if present
    const char *start_ptr = src;
    const char *think_open = strstr(src, "<think>");
    const char *think_close = strstr(src, "</think>");
    if (think_open && think_close && think_close > think_open) {
        start_ptr = think_close + 8; // skip past </think>
    }

    size_t d = 0;
    for (size_t s = 0; start_ptr[s] != '\0' && d < max_len - 1; s++) {
        char c = start_ptr[s];
        // Strip markdown and formatting artifacts
        if (c == '*' || c == '#' || c == '_' || c == '~' || c == '`' || c == '\"' || c == '\\' || c == '<' || c == '>') {
            continue;
        }
        if (c == '\r' || c == '\n') {
            c = ' ';
        }
        if (c == ' ' && d > 0 && dst[d - 1] == ' ') {
            continue;
        }
        dst[d++] = c;
    }
    while (d > 0 && dst[d - 1] == ' ') d--;
    dst[d] = '\0';
}

static bool is_song_play_request(const char *text, char *out_song_name, size_t max_len) {
    if (!text || strlen(text) == 0 || !out_song_name || max_len == 0) return false;
    out_song_name[0] = '\0';

    char stripped[512] = {0};
    strip_vietnamese_diacritics(text, stripped, sizeof(stripped));

    String s = String(stripped);
    s.toLowerCase();

    // STRICT SPECIFIC PREFIXES - Only match explicit music/song requests!
    // NEVER match generic single words like "mo", "bat", "nghe"!
    const char *prefixes[] = {
        "hat cho toi nghe bai hat ", "hat cho toi nghe bai ",
        "hat cho toi bai hat ",      "hat cho toi bai ",
        "mo cho toi nghe bai hat ",  "mo cho toi nghe bai ",
        "bat cho toi nghe bai hat ", "bat cho toi nghe bai ",
        "cho toi nghe bai hat ",     "cho toi nghe bai ",
        "toi muon nghe bai hat ",    "toi muon nghe bai ",
        "muon nghe bai hat ",        "muon nghe bai ",
        "tim kiem bai hat ",         "tim kiem bai ",
        "tim cho toi bai hat ",      "tim cho toi bai ",
        "tim bai hat ",              "tim bai ",
        "tim ca khuc ",              "tim nhac ",
        "hat bai hat ",              "hat bai ",
        "bat bai hat ",              "bat bai ",
        "bat ca khuc ",              "bat nhac ",
        "mo bai hat ",               "mo bai ",
        "mo ca khuc ",               "mo nhac ",
        "phat bai hat ",             "phat bai ",
        "phat ca khuc ",             "phat nhac ",
        "nghe bai hat ",             "nghe bai ",
        "choi bai hat ",             "choi bai ",
        "bai hat "
    };

    for (const char *p : prefixes) {
        int idx = s.indexOf(p);
        if (idx >= 0) {
            String sub = s.substring(idx + strlen(p));
            sub.trim();

            while (sub.endsWith(".") || sub.endsWith("!") || sub.endsWith("?") || sub.endsWith(",")) {
                sub = sub.substring(0, sub.length() - 1);
                sub.trim();
            }

            // Strip trailing courtesy words (e.g. "đi", "nhé", "nào", "hộ tôi", "giúp tôi", "với")
            if (sub.endsWith(" di")) sub = sub.substring(0, sub.length() - 3);
            if (sub.endsWith(" nhe")) sub = sub.substring(0, sub.length() - 4);
            if (sub.endsWith(" nao")) sub = sub.substring(0, sub.length() - 4);
            if (sub.endsWith(" voi")) sub = sub.substring(0, sub.length() - 4);
            if (sub.endsWith(" ho toi")) sub = sub.substring(0, sub.length() - 7);
            if (sub.endsWith(" giup toi")) sub = sub.substring(0, sub.length() - 9);
            if (sub.endsWith(" cho toi")) sub = sub.substring(0, sub.length() - 8);
            sub.trim();

            // If it's a generic singing request (e.g. "hát đi", "hát một bài", "hát gì vui vui"),
            // return false so Gemini AI sings directly!
            if (sub.equals("mot bai") || sub.equals("1 bai") || sub.equals("di") || 
                sub.equals("gi do") || sub.equals("bai gi do") || sub.equals("mot bai gi do") ||
                sub.equals("vui") || sub.equals("vui ve") || sub.equals("hay") ||
                sub.length() < 2) {
                return false;
            }

            strncpy(out_song_name, sub.c_str(), max_len - 1);
            out_song_name[max_len - 1] = '\0';
            return true;
        }
    }

    return false;
}

static volatile bool s_ai_running = false;
static volatile bool s_ai_listening_active = false;
static volatile bool s_ai_stop_listen_req = false;

static const char *s_days_vi[] = {
    "Chủ Nhật", "Thứ Hai", "Thứ Ba", "Thứ Tư", "Thứ Năm", "Thứ Sáu", "Thứ Bảy"
};

static void get_current_time_str(char *time_short, size_t short_sz, char *date_full, size_t full_sz) {
    time_t now = 0;
    time(&now);
    if (now > 1700000000) { // Valid timestamp synced from NTP
        now += 7 * 3600;    // GMT+7 (Vietnam Time)
        struct tm timeinfo;
        gmtime_r(&now, &timeinfo);
        snprintf(time_short, short_sz, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        snprintf(date_full, full_sz, "%02d/%02d/%04d", timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
    } else {
        snprintf(time_short, short_sz, "10:30");
        snprintf(date_full, full_sz, "28/09/2026");
    }
}

static void ai_agent_task_worker(void *pvParam) {
    uint32_t t_turn_start = millis();
    if (!voice_test_buffer) {
        voice_test_buffer = (int16_t *)ps_malloc(AUDIO_SAMPLE_RATE * 11 * sizeof(int16_t));
        if (!voice_test_buffer) {
            voice_test_buffer = (int16_t *)malloc(AUDIO_SAMPLE_RATE * 10 * sizeof(int16_t));
        }
    }

    // Ensure Wi-Fi auto-reconnects immediately if it briefly dropped while idle
    if (WiFi.status() != WL_CONNECTED && cur_wifi_ssid.length() > 0) {
        Serial.printf("[AI] Wi-Fi not connected, triggering fast reconnect to %s...\n", cur_wifi_ssid.c_str());
        WiFi.reconnect();
    }

    // Pre-warm Groq TLS socket in background on Core 0 while user speaks on Core 1!
    if (WiFi.status() == WL_CONNECTED && strlen(GROQ_API_KEY) > 0) {
        GroqAI::prewarm_connection_async();
    }

    // Step 1: Listening State with Smart VAD (auto-stops 0.6s after speech ends)
    if (lbl_ai_status) {
        lv_label_set_text(lbl_ai_status, LV_SYMBOL_AUDIO " ĐANG LẮNG NGHE (TỰ NGẮT)...");
        lv_obj_set_style_text_color(lbl_ai_status, lv_color_hex(0xEF4444), 0);
    }
    if (lbl_ai_bubble) {
        lv_label_set_text(lbl_ai_bubble, "Robot: Đang lắng nghe... Bạn nói xong tự ngắt hoặc chạm Robot để gửi ngay!");
    }
    if (btn_robot_orb) {
        lv_obj_set_style_border_color(btn_robot_orb, lv_color_hex(0xEF4444), 0);
        lv_obj_set_style_shadow_color(btn_robot_orb, lv_color_hex(0xEF4444), 0);
    }

    // Short prompt chime (40ms)
    ES8311_Audio::play_tone(1200, 45, 9000.0f);
    vTaskDelay(pdMS_TO_TICKS(20));

    // Record voice from MEMS mic with Smart VAD (stops automatically after user finishes speaking)
    size_t samples_to_rec = (size_t)(AUDIO_SAMPLE_RATE * AI_RECORD_SECONDS);
    size_t recorded = 0;
    s_ai_stop_listen_req = false;
    s_ai_listening_active = true;
    if (voice_test_buffer) {
        recorded = ES8311_Audio::record_samples_vad(
            voice_test_buffer,
            samples_to_rec,
            on_tts_speech_level,
            &s_ai_stop_listen_req,
            AI_VAD_SILENCE_MS,
            AI_VAD_NO_SPEECH_MS
        );
    }
    s_ai_listening_active = false;
    on_tts_speech_level(-1);

    // Step 2: AI Processing / Thinking State
    if (lbl_ai_status) {
        lv_label_set_text(lbl_ai_status, LV_SYMBOL_REFRESH " ĐANG XỬ LÝ SIÊU TỐC...");
        lv_obj_set_style_text_color(lbl_ai_status, lv_color_hex(0xF59E0B), 0);
    }
    if (lbl_ai_bubble) {
        lv_label_set_text(lbl_ai_bubble, "Robot: Đang nhận diện giọng nói & phản hồi tức thì...");
    }
    if (btn_robot_orb) {
        lv_obj_set_style_border_color(btn_robot_orb, lv_color_hex(0xF59E0B), 0);
        lv_obj_set_style_shadow_color(btn_robot_orb, lv_color_hex(0xF59E0B), 0);
    }

    // Voice Activity Detection (VAD) peak check
    int32_t peak_val = 0;
    for (size_t i = 0; i < recorded; i++) {
        int32_t s = abs(voice_test_buffer[i]);
        if (s > peak_val) peak_val = s;
    }

    char speech_out[1024] = {0};
    char ui_out[1536] = {0};
    bool cloud_answered = false;

    // Wait up to 1.5s if Wi-Fi was in the middle of reconnecting
    if (WiFi.status() != WL_CONNECTED && cur_wifi_ssid.length() > 0) {
        uint32_t w_start = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - w_start < 1500)) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    // Step 2b: Cloud AI (Groq Whisper Turbo STT + LLaMA-3.1 Keep-Alive)
    if (WiFi.status() != WL_CONNECTED) {
        if (lbl_ai_status) {
            lv_label_set_text(lbl_ai_status, LV_SYMBOL_WARNING " CHƯA CÓ WI-FI");
            lv_obj_set_style_text_color(lbl_ai_status, lv_color_hex(0xEF4444), 0);
        }
        strncpy(speech_out, "Chưa kết nối Wi-Fi, bạn hãy vào cài đặt để kết nối mạng nhé", sizeof(speech_out) - 1);
        strncpy(ui_out, "Robot: Chưa kết nối Wi-Fi! Vui lòng vào tab Cài đặt để kết nối Wi-Fi.", sizeof(ui_out) - 1);
        cloud_answered = true;
    } else if (strlen(GROQ_API_KEY) == 0 && strlen(GEMINI_API_KEY) == 0) {
        // Connected to Wi-Fi, but no API Key configured in config_ai.h
        if (lbl_ai_status) {
            lv_label_set_text(lbl_ai_status, LV_SYMBOL_SETTINGS " CẦN AI API KEY");
            lv_obj_set_style_text_color(lbl_ai_status, lv_color_hex(0xF59E0B), 0);
        }
        static int offline_rule_idx = 0;
        offline_rule_idx = (offline_rule_idx + 1) % 4;
        const char *canned_reply = "";
        if (offline_rule_idx == 0) {
            canned_reply = "Nhiệt độ ngăn mát đang là 4 độ C, ngăn đông là âm 18 độ C.";
        } else if (offline_rule_idx == 1) {
            canned_reply = "Rau củ quả trong tủ đang được giữ ẩm rất tốt ở mức 45 phần trăm.";
        } else if (offline_rule_idx == 2) {
            canned_reply = "Hệ thống khử khuẩn Ion Bạc đang hoạt động sạch sẽ.";
        } else {
            canned_reply = "Bạn có muốn gợi ý thực đơn món ăn dinh dưỡng hôm nay không?";
        }
        snprintf(speech_out, sizeof(speech_out), "%s", canned_reply);
        snprintf(ui_out, sizeof(ui_out), "Robot: [Chưa cài API Key] Mẫu: %s", canned_reply);
        cloud_answered = true;
    } else if (peak_val < 180 || recorded < (AUDIO_SAMPLE_RATE / 5)) {
        // Sound too quiet or silence
        if (lbl_ai_status) {
            lv_label_set_text(lbl_ai_status, LV_SYMBOL_WARNING " ÂM THANH QUÁ NHỎ");
            lv_obj_set_style_text_color(lbl_ai_status, lv_color_hex(0xF59E0B), 0);
        }
        strncpy(speech_out, "Tôi chưa nghe rõ, bạn hãy nói to và gần micro hơn nhé", sizeof(speech_out) - 1);
        strncpy(ui_out, "Robot: Chưa nhận được âm thanh rõ. Bạn hãy nói gần Micro hơn nhé.", sizeof(ui_out) - 1);
        cloud_answered = true;
    } else {
        // Live Cloud AI: Groq Whisper-Large-V3-Turbo STT + Keep-Alive LLaMA-3.3 / Gemini Flash
        if (lbl_ai_status) {
            lv_label_set_text(lbl_ai_status, LV_SYMBOL_AUDIO " NHẬN DIỆN GIỌNG NÓI...");
            lv_obj_set_style_text_color(lbl_ai_status, lv_color_hex(0xA855F7), 0); // Purple
        }

        char user_text[512] = {0};
        bool stt_ok = GroqAI::speech_to_text(voice_test_buffer, recorded, user_text, sizeof(user_text));
        if (stt_ok && strlen(user_text) > 0) {
            char clean_user[512] = {0};
            clean_voice_text(clean_user, user_text, sizeof(clean_user));

            // Check if user requested playing a song from Zing MP3
            char song_name[128] = {0};
            if (is_song_play_request(clean_user, song_name, sizeof(song_name))) {
                Serial.printf("[MAIN] Song request recognized: \"%s\"\n", song_name);

                if (lbl_ai_status) {
                    lv_label_set_text(lbl_ai_status, LV_SYMBOL_AUDIO " ĐANG TÌM TRÊN ZING...");
                    lv_obj_set_style_text_color(lbl_ai_status, lv_color_hex(0x38BDF8), 0);
                }
                if (btn_robot_orb) {
                    lv_obj_set_style_border_color(btn_robot_orb, lv_color_hex(0x38BDF8), 0);
                    lv_obj_set_style_shadow_color(btn_robot_orb, lv_color_hex(0x38BDF8), 0);
                }
                if (lbl_ai_bubble) {
                    char tmp[600];
                    snprintf(tmp, sizeof(tmp), "Bạn: \"%s\"\n\nRobot: Đang tìm và phát bài \"%s\"...", clean_user, song_name);
                    lv_label_set_text(lbl_ai_bubble, tmp);
                }

                auto status_ui_cb = [](const char *msg) {
                    if (lbl_ai_bubble && msg) {
                        char full_bubble[600];
                        snprintf(full_bubble, sizeof(full_bubble), "Robot: %s\n\n(Chạm vào Robot để dừng nhạc)", msg);
                        lv_label_set_text(lbl_ai_bubble, full_bubble);
                    }
                };

                bool played = ZingPlayer::play_song_by_query(song_name, on_tts_speech_level, status_ui_cb);
                if (played) {
                    if (lbl_ai_status) {
                        lv_label_set_text(lbl_ai_status, LV_SYMBOL_OK " TRỰC TUYẾN");
                        lv_obj_set_style_text_color(lbl_ai_status, lv_color_hex(0x00E5FF), 0);
                    }
                    s_ai_running = false;
                    vTaskDelete(NULL);
                    return;
                }

                // If Zing MP3 proxy offline or song not found:
                // Fallthrough immediately to AI to sing or answer directly in the SAME turn!
                Serial.println("[MAIN] Zing proxy offline or not found, falling through to AI singing...");
            }

            if (lbl_ai_status) {
#if (AI_ENGINE_PRIMARY == 1)
                lv_label_set_text(lbl_ai_status, LV_SYMBOL_REFRESH " GEMINI ĐANG SUY NGHĨ...");
                lv_obj_set_style_text_color(lbl_ai_status, lv_color_hex(0x38BDF8), 0); // Cyan/Sky
#else
                lv_label_set_text(lbl_ai_status, LV_SYMBOL_REFRESH " TRẢ LỜI TỨC THÌ...");
                lv_obj_set_style_text_color(lbl_ai_status, lv_color_hex(0xF59E0B), 0); // Amber
#endif
            }
            if (lbl_ai_bubble) {
                char tmp[600];
                snprintf(tmp, sizeof(tmp), "Bạn: \"%s\"\n\nRobot: Đang suy nghĩ...", clean_user);
                lv_label_set_text(lbl_ai_bubble, tmp);
            }

            char ai_reply[1024] = {0};
            bool chat_ok = false;

#if (AI_ENGINE_PRIMARY == 1)
            // Ưu tiên Google Gemini
            if (strlen(GEMINI_API_KEY) > 0) {
                chat_ok = GeminiAI::chat_completion(clean_user, ai_reply, sizeof(ai_reply), temp_mat, temp_dong);
            }
            // Fallback sang Groq nếu Gemini gặp sự cố mạng hoặc quota
            if (!chat_ok && strlen(GROQ_API_KEY) > 0) {
                Serial.println("[AI] Gemini failed or busy, falling back to Groq LLaMA-3.3...");
                            char time_ctx_buf[64] = {0};
            char t_sh[16], d_fl[32];
            get_current_time_str(t_sh, sizeof(t_sh), d_fl, sizeof(d_fl));
            time_t now_sec = 0;
            time(&now_sec);
            int wday_idx = 1; // Default Thứ Hai
            if (now_sec > 1700000000) {
                now_sec += 7 * 3600;
                struct tm ti;
                gmtime_r(&now_sec, &ti);
                wday_idx = ti.tm_wday;
            }
            snprintf(time_ctx_buf, sizeof(time_ctx_buf), "%s ngày %s lúc %s", s_days_vi[wday_idx % 7], d_fl, t_sh);
            chat_ok = GroqAI::chat_completion(clean_user, ai_reply, sizeof(ai_reply), temp_mat, temp_dong, time_ctx_buf);
            }
#else
            // Ưu tiên Groq LPU (Tái sử dụng Keep-Alive TLS socket từ STT -> độ trễ ~300ms!)
            if (strlen(GROQ_API_KEY) > 0) {
                            char time_ctx_buf[64] = {0};
            char t_sh[16], d_fl[32];
            get_current_time_str(t_sh, sizeof(t_sh), d_fl, sizeof(d_fl));
            time_t now_sec = 0;
            time(&now_sec);
            int wday_idx = 1; // Default Thứ Hai
            if (now_sec > 1700000000) {
                now_sec += 7 * 3600;
                struct tm ti;
                gmtime_r(&now_sec, &ti);
                wday_idx = ti.tm_wday;
            }
            snprintf(time_ctx_buf, sizeof(time_ctx_buf), "%s ngày %s lúc %s", s_days_vi[wday_idx % 7], d_fl, t_sh);
            chat_ok = GroqAI::chat_completion(clean_user, ai_reply, sizeof(ai_reply), temp_mat, temp_dong, time_ctx_buf);
            }
            if (!chat_ok && strlen(GEMINI_API_KEY) > 0) {
                Serial.println("[AI] Groq failed, falling back to Google Gemini...");
                chat_ok = GeminiAI::chat_completion(clean_user, ai_reply, sizeof(ai_reply), temp_mat, temp_dong);
            }
#endif

            if (chat_ok && strlen(ai_reply) > 0) {
                char clean_reply[1024] = {0};
                clean_voice_text(clean_reply, ai_reply, sizeof(clean_reply));

                strncpy(speech_out, clean_reply, sizeof(speech_out) - 1);
                cloud_answered = true;
                snprintf(ui_out, sizeof(ui_out), "Bạn: \"%s\"\n\nRobot: %s", clean_user, clean_reply);
            } else {
                cloud_answered = true;
                snprintf(ui_out, sizeof(ui_out), "Bạn: \"%s\"\n\nRobot: Lỗi kết nối dịch vụ AI (hết quota hoặc lỗi mạng).", clean_user);
                strncpy(speech_out, "Đã nhận diện giọng nói nhưng không kết nối được máy chủ trí tuệ nhân tạo", sizeof(speech_out) - 1);
            }
        } else {
            cloud_answered = true;
            snprintf(ui_out, sizeof(ui_out), "Robot: Không nhận diện được giọng nói (Kiểm tra Micro hoặc kết nối mạng).");
            strncpy(speech_out, "Tôi chưa nhận diện được câu hỏi của bạn, bạn hãy nói lại nhé", sizeof(speech_out) - 1);
        }
    }

    Serial.printf("[AI] Total turn ready to speak in %lu ms\n", (unsigned long)(millis() - t_turn_start));

    // Step 3: Speaking State with Google TTS Streaming
    if (lbl_ai_status) {
        lv_label_set_text(lbl_ai_status, LV_SYMBOL_VOLUME_MAX " ĐANG TRẢ LỜI...");
        lv_obj_set_style_text_color(lbl_ai_status, lv_color_hex(0x10B981), 0);
    }
    if (btn_robot_orb) {
        lv_obj_set_style_border_color(btn_robot_orb, lv_color_hex(0x10B981), 0);
        lv_obj_set_style_shadow_color(btn_robot_orb, lv_color_hex(0x10B981), 0);
    }
    if (lbl_ai_bubble) {
        lv_label_set_text(lbl_ai_bubble, ui_out);
    }
    if (box_bubble) {
        lv_obj_scroll_to_y(box_bubble, 0, LV_ANIM_OFF);
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // Playback speech with live dancing spectrum (streamed on-the-fly)
    bool spoken = GoogleTTS::speak(speech_out, on_tts_speech_level);
    on_tts_speech_level(-1);
    if (!spoken) {
        ES8311_Audio::play_test_melody();
    }

    // Step 4: Return to Online Idle State
    if (lbl_ai_status) {
        lv_label_set_text(lbl_ai_status, LV_SYMBOL_OK " TRỰC TUYẾN");
        lv_obj_set_style_text_color(lbl_ai_status, lv_color_hex(0x00E5FF), 0);
    }
    if (btn_robot_orb) {
        lv_obj_set_style_border_color(btn_robot_orb, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_shadow_color(btn_robot_orb, lv_color_hex(0x00E5FF), 0);
    }

    s_ai_running = false;
    vTaskDelete(NULL);
}

// 1. Interactive AI Agent Voice Conversation
static void btn_agent_talk_click(lv_event_t *e) {
    s_last_activity_time = millis();
    if (ZingPlayer::is_playing()) {
        ZingPlayer::stop();
        ES8311_Audio::play_touch_sound(800, 30);
        return;
    }
    if (s_ai_running) {
        // Allow user to tap Robot while listening to finish recording immediately!
        if (s_ai_listening_active) {
            s_ai_stop_listen_req = true;
        }
        return;
    }
    s_ai_running = true;

    xTaskCreatePinnedToCore(
        ai_agent_task_worker,
        "ai_task",
        32768,
        NULL,
        2,
        NULL,
        1
    );
}

// 2. Dedicated Voice Record & Playback Test
static void btn_test_record_click(lv_event_t *e) {
    if (!voice_test_buffer) {
        voice_test_buffer = (int16_t *)heap_caps_malloc(AUDIO_SAMPLE_RATE * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!voice_test_buffer) {
            voice_test_buffer = (int16_t *)malloc(AUDIO_SAMPLE_RATE * 2 * sizeof(int16_t));
        }
    }
    if (!voice_test_buffer) {
        if (lbl_ai_bubble) lv_label_set_text(lbl_ai_bubble, "Lỗi: Không đủ bộ nhớ RAM để tạo buffer ghi âm!");
        return;
    }

    if (lbl_ai_status) lv_label_set_text(lbl_ai_status, LV_SYMBOL_AUDIO " ĐANG THU ÂM (2s)...");
    if (lbl_ai_bubble) lv_label_set_text(lbl_ai_bubble, "Robot: Hãy nói vào Micro ngay bây giờ! Đang thu 2 giây...");
    if (btn_robot_orb) {
        lv_obj_set_style_border_color(btn_robot_orb, lv_color_hex(0xEF4444), 0);
        lv_obj_set_style_shadow_color(btn_robot_orb, lv_color_hex(0xEF4444), 0);
    }
    lv_refr_now(NULL);

    ES8311_Audio::play_touch_sound(1200, 60);
    delay(40);

    // Record with live spectrum visualization
    size_t recorded = ES8311_Audio::record_samples(voice_test_buffer, AUDIO_SAMPLE_RATE * 2, update_spectrum_ui);
    update_spectrum_ui(0);

    if (lbl_ai_status) lv_label_set_text(lbl_ai_status, LV_SYMBOL_VOLUME_MAX " ĐANG PHÁT LẠI...");
    if (lbl_ai_bubble) lv_label_set_text(lbl_ai_bubble, "Robot: Đang phát lại chính xác giọng nói của bạn qua loa...");
    if (btn_robot_orb) {
        lv_obj_set_style_border_color(btn_robot_orb, lv_color_hex(0x10B981), 0);
        lv_obj_set_style_shadow_color(btn_robot_orb, lv_color_hex(0x10B981), 0);
    }
    lv_refr_now(NULL);
    delay(50);

    // Playback with live spectrum visualization
    ES8311_Audio::play_samples(voice_test_buffer, recorded, update_spectrum_ui);
    update_spectrum_ui(0);

    if (lbl_ai_status) lv_label_set_text(lbl_ai_status, LV_SYMBOL_OK " TRỰC TUYẾN");
    if (lbl_ai_bubble) lv_label_set_text(lbl_ai_bubble, "Robot: Thu & Phát hoàn tất! Cả Micro và Loa đều hoạt động tốt.");
    if (btn_robot_orb) {
        lv_obj_set_style_border_color(btn_robot_orb, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_shadow_color(btn_robot_orb, lv_color_hex(0x00E5FF), 0);
    }
}

// 3. Live 15-Bar Equalizer Spectrum & VU-Meter Timer
static void mic_visualizer_timer_cb(lv_timer_t *timer) {
    if (!tv) return;
    if (lv_tabview_get_tab_act(tv) != 2) return; // Only process on Tab 3 (AI Assistant)

    // If AI recording/TTS/Zing is feeding live levels via s_tts_live_level, render safely on LVGL thread
    if (s_tts_live_level >= 0) {
        update_spectrum_ui(s_tts_live_level);
        return;
    }

    if (s_ai_running || ZingPlayer::is_playing()) {
        update_spectrum_ui(0);
        return; // CRITICAL: Never steal I2S samples during AI recording/playback!
    }

    int level = ES8311_Audio::read_mic_level();
    update_spectrum_ui(level);
}

/* ====================================================================
 * PROFESSIONAL FULL-SCREEN WI-FI MANAGER (PAGE 4)
 * ==================================================================== */
static void populate_wifi_list_ui();
static void open_password_sheet(const char *ssid, bool is_open, bool is_manual = false);
static void open_connected_network_dialog(const char *ssid, int rssi);
static void update_current_wifi_card_ui();
static void close_wifi_manager_screen(lv_event_t *e = nullptr);

static bool s_wifi_scanning_active = false;
static lv_timer_t *s_wifi_scan_poll_timer = nullptr;
static uint32_t s_wifi_scan_start_ms = 0;

static void wifi_scan_poll_timer_cb(lv_timer_t *timer) {
    int16_t status = WiFi.scanComplete();

    // Prevent hanging: timeout after 6.5 seconds
    if (status == WIFI_SCAN_RUNNING && (millis() - s_wifi_scan_start_ms > 6500)) {
        Serial.println("[WIFI-SCAN] Scan timed out after 6.5s");
        status = WIFI_SCAN_FAILED;
    }

    if (status == WIFI_SCAN_RUNNING) {
        return; // Still in progress, check on next timer tick
    }

    Serial.printf("[WIFI-SCAN] Scan complete with status = %d\n", status);

    if (s_wifi_scan_poll_timer) {
        lv_timer_del(s_wifi_scan_poll_timer);
        s_wifi_scan_poll_timer = nullptr;
    }
    s_wifi_scanning_active = false;
    WiFi.setAutoReconnect(true);

    if (status > 0) {
        s_scanned_count = 0;
        for (int i = 0; i < status && s_scanned_count < 30; i++) {
            String s = WiFi.SSID(i);
            if (s.length() == 0) continue;
            bool dup = false;
            for (int k = 0; k < s_scanned_count; k++) {
                if (strcmp(s_scanned_wifis[k].ssid, s.c_str()) == 0) {
                    if (WiFi.RSSI(i) > s_scanned_wifis[k].rssi) {
                        s_scanned_wifis[k].rssi = WiFi.RSSI(i);
                        s_scanned_wifis[k].is_open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
                    }
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                strncpy(s_scanned_wifis[s_scanned_count].ssid, s.c_str(), sizeof(s_scanned_wifis[0].ssid) - 1);
                s_scanned_wifis[s_scanned_count].rssi = WiFi.RSSI(i);
                s_scanned_wifis[s_scanned_count].is_open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
                s_scanned_count++;
            }
        }
    }

    // Always ensure known / saved networks are present
    for (size_t i = 0; i < sizeof(s_known_wifis) / sizeof(s_known_wifis[0]) && s_scanned_count < 30; i++) {
        if (!s_known_wifis[i].ssid || strlen(s_known_wifis[i].ssid) == 0) continue;
        bool exists = false;
        for (int k = 0; k < s_scanned_count; k++) {
            if (strcmp(s_scanned_wifis[k].ssid, s_known_wifis[i].ssid) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            strncpy(s_scanned_wifis[s_scanned_count].ssid, s_known_wifis[i].ssid, sizeof(s_scanned_wifis[0].ssid) - 1);
            s_scanned_wifis[s_scanned_count].rssi = -60;
            s_scanned_wifis[s_scanned_count].is_open = false;
            s_scanned_count++;
        }
    }

    // Sort by priority (connected first, known next, then rssi)
    auto get_score = [](const char *ssid, int rssi) -> int {
        if (WiFi.status() == WL_CONNECTED && cur_wifi_ssid.equals(ssid)) {
            return 2000 + rssi;
        }
        String p;
        if (is_saved_or_known_wifi(ssid, p)) {
            return 1000 + rssi;
        }
        return rssi;
    };

    for (int i = 0; i < s_scanned_count - 1; i++) {
        for (int j = i + 1; j < s_scanned_count; j++) {
            if (get_score(s_scanned_wifis[j].ssid, s_scanned_wifis[j].rssi) >
                get_score(s_scanned_wifis[i].ssid, s_scanned_wifis[i].rssi)) {
                ScannedWiFi tmp = s_scanned_wifis[i];
                s_scanned_wifis[i] = s_scanned_wifis[j];
                s_scanned_wifis[j] = tmp;
            }
        }
    }

    WiFi.scanDelete();
    WiFi.setAutoReconnect(true);
    populate_wifi_list_ui();
}

static void scan_wifi_and_populate_list() {
    if (s_wifi_scanning_active) return;
    s_wifi_scanning_active = true;

    reload_nvs_wifi_cache();

    // Disable background reconnect briefly while scanning to avoid RF collision
    WiFi.setAutoReconnect(false);
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(false, false);
    }
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    if (lbl_wifi_scan_count) {
        lv_label_set_text(lbl_wifi_scan_count, LV_SYMBOL_REFRESH " Đang quét sóng Wi-Fi (2.4GHz)...");
        lv_obj_set_style_text_color(lbl_wifi_scan_count, lv_color_hex(0xFBBF24), 0);
    }
    if (btn_wifi_rescan) {
        lv_obj_add_state(btn_wifi_rescan, LV_STATE_DISABLED);
        if (lbl_wifi_rescan) {
            lv_label_set_text(lbl_wifi_rescan, LV_SYMBOL_REFRESH " ...");
            lv_obj_set_style_text_color(lbl_wifi_rescan, lv_color_hex(0x64748B), 0);
        }
    }

    WiFi.scanDelete();

    // Start non-blocking async scan
    int16_t ret = WiFi.scanNetworks(true, true, false, 150);
    Serial.printf("[WIFI-SCAN] Async scanNetworks started, code=%d\n", ret);

    s_wifi_scan_start_ms = millis();
    if (s_wifi_scan_poll_timer) {
        lv_timer_del(s_wifi_scan_poll_timer);
        s_wifi_scan_poll_timer = nullptr;
    }
    s_wifi_scan_poll_timer = lv_timer_create(wifi_scan_poll_timer_cb, 200, NULL);
}

static void start_wifi_connection_async(const char *ssid, const char *pass);

static void wifi_network_item_click(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_scanned_count) return;
    ES8311_Audio::play_touch_sound(1200, 30);

    if (WiFi.status() == WL_CONNECTED && cur_wifi_ssid.equals(s_scanned_wifis[idx].ssid)) {
        open_connected_network_dialog(s_scanned_wifis[idx].ssid, s_scanned_wifis[idx].rssi);
        return;
    }

    open_password_sheet(s_scanned_wifis[idx].ssid, s_scanned_wifis[idx].is_open);

    // If this network already has a saved password in NVS, auto-connect immediately without forcing re-typing!
    String saved_pass;
    if (!s_scanned_wifis[idx].is_open && is_saved_or_known_wifi(s_scanned_wifis[idx].ssid, saved_pass)) {
        Serial.printf("[WIFI] Auto-connecting to saved network: %s\n", s_scanned_wifis[idx].ssid);
        start_wifi_connection_async(s_scanned_wifis[idx].ssid, saved_pass.c_str());
    }
}

static void populate_wifi_list_ui() {
    if (!wifi_list_cont) return;
    lv_obj_clean(wifi_list_cont);

    if (btn_wifi_rescan) {
        lv_obj_clear_state(btn_wifi_rescan, LV_STATE_DISABLED);
        if (lbl_wifi_rescan) {
            lv_label_set_text(lbl_wifi_rescan, LV_SYMBOL_REFRESH " Quét");
            lv_obj_set_style_text_color(lbl_wifi_rescan, lv_color_hex(0x00E5FF), 0);
        }
    }

    if (lbl_wifi_scan_count) {
        if (s_scanned_count > 0) {
            lv_label_set_text_fmt(lbl_wifi_scan_count, "MẠNG KHẢ DỤNG (%d mạng tìm thấy)", s_scanned_count);
            lv_obj_set_style_text_color(lbl_wifi_scan_count, lv_color_hex(0x94A3B8), 0);
        } else {
            lv_label_set_text(lbl_wifi_scan_count, "Không tìm thấy mạng! Bấm [Quét] để thử lại.");
            lv_obj_set_style_text_color(lbl_wifi_scan_count, lv_color_hex(0xEF4444), 0);
        }
    }

    for (int i = 0; i < s_scanned_count; i++) {
        bool is_current = (WiFi.status() == WL_CONNECTED && cur_wifi_ssid.equals(s_scanned_wifis[i].ssid));
        String known_pass;
        bool is_known = is_saved_or_known_wifi(s_scanned_wifis[i].ssid, known_pass);
        int sig_pct = constrain(2 * (s_scanned_wifis[i].rssi + 100), 0, 100);

        lv_obj_t *item = lv_btn_create(wifi_list_cont);
        lv_obj_set_size(item, 296, 50);
        if (is_current) {
            lv_obj_set_style_bg_color(item, lv_color_hex(0x0B2421), 0);
            lv_obj_set_style_border_color(item, lv_color_hex(0x10B981), 0);
        } else if (is_known) {
            lv_obj_set_style_bg_color(item, lv_color_hex(0x0C1C36), 0);
            lv_obj_set_style_border_color(item, lv_color_hex(0x0284C7), 0);
        } else {
            lv_obj_set_style_bg_color(item, lv_color_hex(0x0E172A), 0);
            lv_obj_set_style_border_color(item, lv_color_hex(0x1E2E4E), 0);
        }
        lv_obj_set_style_bg_color(item, lv_color_hex(0x1E2E4E), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(item, 1, 0);
        lv_obj_set_style_radius(item, 10, 0);
        lv_obj_set_style_pad_all(item, 6, 0);
        lv_obj_add_event_cb(item, wifi_network_item_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        // Signal icon
        lv_obj_t *ic_sig = lv_label_create(item);
        lv_label_set_text(ic_sig, LV_SYMBOL_WIFI);
        if (s_scanned_wifis[i].rssi > -65) {
            lv_obj_set_style_text_color(ic_sig, lv_color_hex(0x10B981), 0);
        } else if (s_scanned_wifis[i].rssi > -78) {
            lv_obj_set_style_text_color(ic_sig, lv_color_hex(0xFBBF24), 0);
        } else {
            lv_obj_set_style_text_color(ic_sig, lv_color_hex(0x94A3B8), 0);
        }
        lv_obj_align(ic_sig, LV_ALIGN_LEFT_MID, 6, 0);

        // SSID Name Label
        lv_obj_t *lbl_ssid = lv_label_create(item);
        char ssid_clean[34] = {0};
        strip_vietnamese_diacritics(s_scanned_wifis[i].ssid, ssid_clean, sizeof(ssid_clean));
        lv_label_set_text(lbl_ssid, ssid_clean);
        lv_obj_set_style_text_color(lbl_ssid, is_current ? lv_color_hex(0x10B981) : (is_known ? lv_color_hex(0x38BDF8) : lv_color_hex(0xFFFFFF)), 0);
        lv_obj_align(lbl_ssid, LV_ALIGN_LEFT_MID, 34, -8);

        // Security / status subtext
        lv_obj_t *lbl_sub = lv_label_create(item);
        if (is_current) {
            lv_label_set_text_fmt(lbl_sub, "Đang kết nối  •  Sóng %d%%", sig_pct);
            lv_obj_set_style_text_color(lbl_sub, lv_color_hex(0x10B981), 0);
        } else if (s_scanned_wifis[i].is_open) {
            lv_label_set_text_fmt(lbl_sub, "Mạng mở  •  Sóng %d%%", sig_pct);
            lv_obj_set_style_text_color(lbl_sub, lv_color_hex(0x34D399), 0);
        } else if (is_known) {
            lv_label_set_text_fmt(lbl_sub, "Đã lưu (Chạm tự kết nối)  •  Sóng %d%%", sig_pct);
            lv_obj_set_style_text_color(lbl_sub, lv_color_hex(0x38BDF8), 0);
        } else {
            lv_label_set_text_fmt(lbl_sub, "Bảo mật WPA2  •  Sóng %d%%", sig_pct);
            lv_obj_set_style_text_color(lbl_sub, lv_color_hex(0x64748B), 0);
        }
        lv_obj_align(lbl_sub, LV_ALIGN_LEFT_MID, 34, 10);

        // Right side icon
        lv_obj_t *ic_right = lv_label_create(item);
        if (is_current) {
            lv_label_set_text(ic_right, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(ic_right, lv_color_hex(0x10B981), 0);
        } else if (is_known) {
            lv_label_set_text(ic_right, LV_SYMBOL_RIGHT);
            lv_obj_set_style_text_color(ic_right, lv_color_hex(0x38BDF8), 0);
        } else {
            lv_label_set_text(ic_right, LV_SYMBOL_RIGHT);
            lv_obj_set_style_text_color(ic_right, lv_color_hex(0x64748B), 0);
        }
        lv_obj_align(ic_right, LV_ALIGN_RIGHT_MID, -6, 0);
    }

    // Always add manual network button at bottom
    lv_obj_t *btn_add_manual = lv_btn_create(wifi_list_cont);
    lv_obj_set_size(btn_add_manual, 296, 44);
    lv_obj_set_style_bg_color(btn_add_manual, lv_color_hex(0x0C1C36), 0);
    lv_obj_set_style_border_color(btn_add_manual, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(btn_add_manual, 1, 0);
    lv_obj_set_style_radius(btn_add_manual, 10, 0);
    lv_obj_set_style_pad_all(btn_add_manual, 6, 0);
    lv_obj_t *lbl_add_m = lv_label_create(btn_add_manual);
    lv_label_set_text(lbl_add_m, LV_SYMBOL_PLUS "  Thêm mạng Wi-Fi khác (Thủ công)");
    lv_obj_set_style_text_color(lbl_add_m, lv_color_hex(0x00E5FF), 0);
    lv_obj_center(lbl_add_m);
    lv_obj_add_event_cb(btn_add_manual, [](lv_event_t *e){
        open_password_sheet("", false, true);
    }, LV_EVENT_CLICKED, NULL);
}

static void update_current_wifi_card_ui() {
    if (!wifi_current_card || !lbl_wifi_curr_title || !lbl_wifi_curr_detail) return;
    if (WiFi.status() == WL_CONNECTED) {
        lv_obj_set_style_border_color(wifi_current_card, lv_color_hex(0x10B981), 0);
        lv_obj_set_style_bg_color(wifi_current_card, lv_color_hex(0x0A231C), 0);
        lv_label_set_text_fmt(lbl_wifi_curr_title, LV_SYMBOL_WIFI " %s  (Đã kết nối)", cur_wifi_ssid.c_str());
        lv_obj_set_style_text_color(lbl_wifi_curr_title, lv_color_hex(0x10B981), 0);
        int pct = constrain(2 * (WiFi.RSSI() + 100), 0, 100);
        lv_label_set_text_fmt(lbl_wifi_curr_detail, "IP: %s  •  Sóng: %d%%  " LV_SYMBOL_RIGHT, WiFi.localIP().toString().c_str(), pct);
        lv_obj_set_style_text_color(lbl_wifi_curr_detail, lv_color_hex(0x94A3B8), 0);
    } else {
        lv_obj_set_style_border_color(wifi_current_card, lv_color_hex(0x1E2E4E), 0);
        lv_obj_set_style_bg_color(wifi_current_card, lv_color_hex(0x0D1D35), 0);
        lv_label_set_text(lbl_wifi_curr_title, LV_SYMBOL_WARNING " Chưa kết nối mạng Wi-Fi");
        lv_obj_set_style_text_color(lbl_wifi_curr_title, lv_color_hex(0xFBBF24), 0);
        lv_label_set_text(lbl_wifi_curr_detail, "Bấm [Quét] hoặc chọn một mạng bên dưới");
        lv_obj_set_style_text_color(lbl_wifi_curr_detail, lv_color_hex(0x64748B), 0);
    }
}

// Moved get_current_time_str & s_days_vi earlier

static void update_time_ui() {
    bool connected = (WiFi.status() == WL_CONNECTED);
    char t_short[16];
    char d_full[32];
    get_current_time_str(t_short, sizeof(t_short), d_full, sizeof(d_full));

    for (lv_obj_t *lbl : {lbl_time1, lbl_time2, lbl_time3, lbl_time4}) {
        if (lbl) {
            if (connected) {
                lv_label_set_text_fmt(lbl, LV_SYMBOL_WIFI "  %s\n%s", t_short, d_full);
            } else {
                lv_label_set_text_fmt(lbl, "%s\n%s", t_short, d_full);
            }
        }
    }
}

static void update_wifi_ui_status() {
    bool connected = (WiFi.status() == WL_CONNECTED);

    update_time_ui();

    if (lbl_rw_wifi) {
        if (connected) {
            lv_label_set_text_fmt(lbl_rw_wifi, LV_SYMBOL_WIFI " Wi-Fi: %s (Đã kết nối)  " LV_SYMBOL_RIGHT, cur_wifi_ssid.c_str());
            lv_obj_set_style_text_color(lbl_rw_wifi, lv_color_hex(0x10B981), 0);
        } else {
            if (cur_wifi_ssid.length() > 0) {
                lv_label_set_text_fmt(lbl_rw_wifi, "Wi-Fi: %s (Đang kết nối...)  " LV_SYMBOL_RIGHT, cur_wifi_ssid.c_str());
                lv_obj_set_style_text_color(lbl_rw_wifi, lv_color_hex(0xFBBF24), 0);
            } else {
                lv_label_set_text(lbl_rw_wifi, "Wi-Fi: Chưa kết nối  " LV_SYMBOL_RIGHT);
                lv_obj_set_style_text_color(lbl_rw_wifi, lv_color_hex(0x94A3B8), 0);
            }
        }
    }
}

static void wifi_connect_timer_cb(lv_timer_t *t) {
    s_wifi_conn_ticks++;
    int remaining_sec = 6 - (s_wifi_conn_ticks * 250) / 1000;
    if (remaining_sec < 0) remaining_sec = 0;

    if (lbl_wifi_pw_status) {
        lv_label_set_text_fmt(lbl_wifi_pw_status, "Đang xác thực kết nối (%ds)...", remaining_sec);
        lv_obj_set_style_text_color(lbl_wifi_pw_status, lv_color_hex(0xFBBF24), 0);
    }

    if (WiFi.status() == WL_CONNECTED) {
        lv_timer_del(t);
        s_wifi_conn_timer = nullptr;

        save_wifi_credentials_to_nvs(s_connecting_ssid, s_connecting_pass);
        cur_wifi_ssid = String(s_connecting_ssid);
        cur_wifi_pass = String(s_connecting_pass);
        WiFi.setAutoReconnect(true);
        WiFi.setSleep(false);
        esp_wifi_set_ps(WIFI_PS_NONE);

        if (lbl_wifi_pw_status) {
            lv_label_set_text_fmt(lbl_wifi_pw_status, LV_SYMBOL_OK " Đã kết nối! IP: %s", WiFi.localIP().toString().c_str());
            lv_obj_set_style_text_color(lbl_wifi_pw_status, lv_color_hex(0x10B981), 0);
        }
        if (btn_wifi_pw_connect) {
            lv_obj_clear_state(btn_wifi_pw_connect, LV_STATE_DISABLED);
            if (lbl_wifi_pw_connect) lv_label_set_text(lbl_wifi_pw_connect, LV_SYMBOL_OK " Thành công");
        }
        ES8311_Audio::play_touch_sound(1600, 60);

        lv_timer_t *tmr_close = lv_timer_create([](lv_timer_t *tm){
            if (wifi_pw_sheet) {
                lv_obj_del(wifi_pw_sheet);
                wifi_pw_sheet = nullptr;
            }
            update_current_wifi_card_ui();
            populate_wifi_list_ui();
            update_wifi_ui_status();
            lv_timer_del(tm);
        }, 1200, NULL);
        return;
    }

    // Timeout: 6.0 seconds (24 ticks x 250ms = 6000ms) without connection
    // Report failure cleanly on UI - NEVER close the screen or kick user out to Home!
    if (s_wifi_conn_ticks >= 24) {
        lv_timer_del(t);
        s_wifi_conn_timer = nullptr;

        WiFi.disconnect(false, false);
        ES8311_Audio::play_touch_sound(400, 80);
        Serial.println("[WIFI] Connection timeout. Staying in WiFi manager.");

        if (lbl_wifi_pw_status) {
            lv_label_set_text(lbl_wifi_pw_status, LV_SYMBOL_CLOSE " Thất bại! Sai mật khẩu hoặc sóng yếu.");
            lv_obj_set_style_text_color(lbl_wifi_pw_status, lv_color_hex(0xEF4444), 0);
        }
        if (btn_wifi_pw_connect) {
            lv_obj_clear_state(btn_wifi_pw_connect, LV_STATE_DISABLED);
            if (lbl_wifi_pw_connect) lv_label_set_text(lbl_wifi_pw_connect, LV_SYMBOL_REFRESH " Thử lại");
        }
        update_current_wifi_card_ui();
        populate_wifi_list_ui();
        update_wifi_ui_status();
    }
}

static void start_wifi_connection_async(const char *ssid, const char *pass) {
    if (s_wifi_conn_timer) {
        lv_timer_del(s_wifi_conn_timer);
        s_wifi_conn_timer = nullptr;
    }
    strncpy(s_connecting_ssid, ssid, sizeof(s_connecting_ssid) - 1);
    strncpy(s_connecting_pass, pass ? pass : "", sizeof(s_connecting_pass) - 1);
    s_wifi_conn_ticks = 0;

    if (lbl_wifi_pw_status) {
        lv_label_set_text(lbl_wifi_pw_status, "Đang bắt đầu kết nối...");
        lv_obj_set_style_text_color(lbl_wifi_pw_status, lv_color_hex(0xFBBF24), 0);
    }
    if (btn_wifi_pw_connect) {
        lv_obj_add_state(btn_wifi_pw_connect, LV_STATE_DISABLED);
        if (lbl_wifi_pw_connect) lv_label_set_text(lbl_wifi_pw_connect, LV_SYMBOL_REFRESH " Đang kết nối...");
    }

    WiFi.disconnect(false, false);
    delay(40);
    WiFi.mode(WIFI_STA);
    WiFi.persistent(true);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    if (strlen(s_connecting_pass) > 0) {
        WiFi.begin(s_connecting_ssid, s_connecting_pass);
    } else {
        WiFi.begin(s_connecting_ssid);
    }
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_wifi_conn_timer = lv_timer_create(wifi_connect_timer_cb, 250, NULL);
}

static void open_connected_network_dialog(const char *ssid, int rssi) {
    if (wifi_pw_sheet) return;

    wifi_pw_sheet = lv_obj_create(wifi_full_screen ? wifi_full_screen : lv_scr_act());
    lv_obj_set_size(wifi_pw_sheet, 304, 210);
    lv_obj_center(wifi_pw_sheet);
    lv_obj_set_style_bg_color(wifi_pw_sheet, lv_color_hex(0x0A1124), 0);
    lv_obj_set_style_border_color(wifi_pw_sheet, lv_color_hex(0x10B981), 0);
    lv_obj_set_style_border_width(wifi_pw_sheet, 2, 0);
    lv_obj_set_style_radius(wifi_pw_sheet, 14, 0);
    lv_obj_set_style_pad_all(wifi_pw_sheet, 12, 0);
    lv_obj_clear_flag(wifi_pw_sheet, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_t = lv_label_create(wifi_pw_sheet);
    lv_label_set_text(lbl_t, LV_SYMBOL_WIFI " THÔNG TIN MẠNG HIỆN TẠI");
    lv_obj_set_style_text_color(lbl_t, lv_color_hex(0x10B981), 0);
    lv_obj_align(lbl_t, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *lbl_info = lv_label_create(wifi_pw_sheet);
    char buf[160];
    int sig_pct = constrain(2 * (rssi + 100), 0, 100);
    snprintf(buf, sizeof(buf), "Mạng: %s\nIP: %s\nCường độ: %d%% (%d dBm)\nTrạng thái: Đang hoạt động",
             ssid, WiFi.localIP().toString().c_str(), sig_pct, rssi);
    lv_label_set_text(lbl_info, buf);
    lv_obj_set_style_text_color(lbl_info, lv_color_hex(0xE2E8F0), 0);
    lv_obj_align(lbl_info, LV_ALIGN_TOP_LEFT, 0, 30);

    // Nút Ngắt kết nối (Đỏ)
    lv_obj_t *btn_disc = lv_btn_create(wifi_pw_sheet);
    lv_obj_set_size(btn_disc, 134, 38);
    lv_obj_align(btn_disc, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(btn_disc, lv_color_hex(0xDC2626), 0);
    lv_obj_set_style_radius(btn_disc, 8, 0);
    lv_obj_t *l_disc = lv_label_create(btn_disc);
    lv_label_set_text(l_disc, "Ngắt kết nối");
    lv_obj_center(l_disc);
    lv_obj_add_event_cb(btn_disc, [](lv_event_t *e){
        WiFi.disconnect(false, false);
        cur_wifi_ssid = "";
        cur_wifi_pass = "";
        wifi_prefs.begin("wifi_cfg", false);
        wifi_prefs.putString("ssid", "");
        wifi_prefs.putString("pass", "");
        wifi_prefs.end();
        if (wifi_pw_sheet) { lv_obj_del(wifi_pw_sheet); wifi_pw_sheet = nullptr; }
        update_current_wifi_card_ui();
        populate_wifi_list_ui();
        update_wifi_ui_status();
        ES8311_Audio::play_touch_sound(400, 60);
    }, LV_EVENT_CLICKED, NULL);

    // Nút Đóng
    lv_obj_t *btn_cl = lv_btn_create(wifi_pw_sheet);
    lv_obj_set_size(btn_cl, 134, 38);
    lv_obj_align(btn_cl, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_cl, lv_color_hex(0x334155), 0);
    lv_obj_set_style_radius(btn_cl, 8, 0);
    lv_obj_t *l_cl = lv_label_create(btn_cl);
    lv_label_set_text(l_cl, "Đóng");
    lv_obj_center(l_cl);
    lv_obj_add_event_cb(btn_cl, [](lv_event_t *e){
        if (wifi_pw_sheet) { lv_obj_del(wifi_pw_sheet); wifi_pw_sheet = nullptr; }
    }, LV_EVENT_CLICKED, NULL);
}

static void open_password_sheet(const char *ssid, bool is_open, bool is_manual) {
    if (wifi_pw_sheet) return;
    if (ssid) {
        strncpy(s_selected_ssid, ssid, sizeof(s_selected_ssid) - 1);
    } else {
        s_selected_ssid[0] = '\0';
    }

    if (is_open && !is_manual) {
        wifi_pw_sheet = lv_obj_create(wifi_full_screen ? wifi_full_screen : lv_scr_act());
        lv_obj_set_size(wifi_pw_sheet, 304, 190);
        lv_obj_center(wifi_pw_sheet);
        lv_obj_set_style_bg_color(wifi_pw_sheet, lv_color_hex(0x0A1124), 0);
        lv_obj_set_style_border_color(wifi_pw_sheet, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_border_width(wifi_pw_sheet, 2, 0);
        lv_obj_set_style_radius(wifi_pw_sheet, 14, 0);
        lv_obj_set_style_pad_all(wifi_pw_sheet, 12, 0);
        lv_obj_clear_flag(wifi_pw_sheet, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl_t = lv_label_create(wifi_pw_sheet);
        lv_label_set_text(lbl_t, LV_SYMBOL_WIFI " KẾT NỐI MẠNG MỞ");
        lv_obj_set_style_text_color(lbl_t, lv_color_hex(0x00E5FF), 0);
        lv_obj_align(lbl_t, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *lbl_info = lv_label_create(wifi_pw_sheet);
        char buf[128];
        snprintf(buf, sizeof(buf), "Mạng \"%s\" không có mật khẩu.\nBạn có muốn kết nối ngay không?", s_selected_ssid);
        lv_label_set_text(lbl_info, buf);
        lv_obj_set_style_text_color(lbl_info, lv_color_hex(0xE2E8F0), 0);
        lv_obj_align(lbl_info, LV_ALIGN_TOP_LEFT, 0, 30);

        lbl_wifi_pw_status = lv_label_create(wifi_pw_sheet);
        lv_label_set_text(lbl_wifi_pw_status, "");
        lv_obj_align(lbl_wifi_pw_status, LV_ALIGN_TOP_LEFT, 0, 80);

        btn_wifi_pw_connect = lv_btn_create(wifi_pw_sheet);
        lv_obj_set_size(btn_wifi_pw_connect, 134, 38);
        lv_obj_align(btn_wifi_pw_connect, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_bg_color(btn_wifi_pw_connect, lv_color_hex(0x059669), 0);
        lv_obj_set_style_radius(btn_wifi_pw_connect, 8, 0);
        lbl_wifi_pw_connect = lv_label_create(btn_wifi_pw_connect);
        lv_label_set_text(lbl_wifi_pw_connect, LV_SYMBOL_OK " Kết nối");
        lv_obj_center(lbl_wifi_pw_connect);
        lv_obj_add_event_cb(btn_wifi_pw_connect, [](lv_event_t *e){
            start_wifi_connection_async(s_selected_ssid, "");
        }, LV_EVENT_CLICKED, NULL);

        lv_obj_t *btn_cl = lv_btn_create(wifi_pw_sheet);
        lv_obj_set_size(btn_cl, 134, 38);
        lv_obj_align(btn_cl, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_set_style_bg_color(btn_cl, lv_color_hex(0x334155), 0);
        lv_obj_set_style_radius(btn_cl, 8, 0);
        lv_obj_t *l_cl = lv_label_create(btn_cl);
        lv_label_set_text(l_cl, LV_SYMBOL_CLOSE " Hủy");
        lv_obj_center(l_cl);
        lv_obj_add_event_cb(btn_cl, [](lv_event_t *e){
            if (wifi_pw_sheet) { lv_obj_del(wifi_pw_sheet); wifi_pw_sheet = nullptr; }
        }, LV_EVENT_CLICKED, NULL);
        return;
    }

    wifi_pw_sheet = lv_obj_create(wifi_full_screen ? wifi_full_screen : lv_scr_act());
    lv_obj_set_size(wifi_pw_sheet, 308, 440);
    lv_obj_center(wifi_pw_sheet);
    lv_obj_set_style_bg_color(wifi_pw_sheet, lv_color_hex(0x0A1124), 0);
    lv_obj_set_style_border_color(wifi_pw_sheet, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(wifi_pw_sheet, 2, 0);
    lv_obj_set_style_radius(wifi_pw_sheet, 14, 0);
    lv_obj_set_style_pad_all(wifi_pw_sheet, 10, 0);
    lv_obj_clear_flag(wifi_pw_sheet, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *lbl_t = lv_label_create(wifi_pw_sheet);
    lv_label_set_text(lbl_t, is_manual ? LV_SYMBOL_PLUS " THÊM MẠNG THỦ CÔNG" : LV_SYMBOL_WIFI " KẾT NỐI WI-FI");
    lv_obj_set_style_text_color(lbl_t, lv_color_hex(0x00E5FF), 0);
    lv_obj_align(lbl_t, LV_ALIGN_TOP_LEFT, 4, 4);

    // Target SSID Text Area or Label
    static lv_obj_t *ta_manual_ssid = nullptr;
    if (is_manual) {
        ta_manual_ssid = lv_textarea_create(wifi_pw_sheet);
        lv_obj_set_size(ta_manual_ssid, 288, 34);
        lv_obj_align(ta_manual_ssid, LV_ALIGN_TOP_LEFT, 4, 26);
        lv_textarea_set_placeholder_text(ta_manual_ssid, "Tên Wi-Fi (SSID)...");
        lv_textarea_set_one_line(ta_manual_ssid, true);
        lv_obj_set_style_bg_color(ta_manual_ssid, lv_color_hex(0x13203C), 0);
        lv_obj_set_style_text_color(ta_manual_ssid, lv_color_hex(0x00E5FF), 0);
        lv_obj_add_event_cb(ta_manual_ssid, [](lv_event_t *e){
            if (kb_wifi_pw && ta_manual_ssid) lv_keyboard_set_textarea(kb_wifi_pw, ta_manual_ssid);
        }, LV_EVENT_FOCUSED, NULL);
    } else {
        ta_manual_ssid = nullptr;
        lv_obj_t *lbl_net = lv_label_create(wifi_pw_sheet);
        char net_buf[48];
        snprintf(net_buf, sizeof(net_buf), "Mạng: %s", s_selected_ssid);
        lv_label_set_text(lbl_net, net_buf);
        lv_obj_set_style_text_color(lbl_net, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(lbl_net, LV_ALIGN_TOP_LEFT, 4, 28);
    }

    // Password Text Area
    ta_wifi_pw = lv_textarea_create(wifi_pw_sheet);
    lv_obj_set_size(ta_wifi_pw, 236, 36);
    lv_obj_align(ta_wifi_pw, LV_ALIGN_TOP_LEFT, 4, is_manual ? 64 : 56);
    lv_textarea_set_placeholder_text(ta_wifi_pw, "Nhập mật khẩu...");
    lv_obj_add_event_cb(ta_wifi_pw, [](lv_event_t *e){
        if (kb_wifi_pw && ta_wifi_pw) lv_keyboard_set_textarea(kb_wifi_pw, ta_wifi_pw);
    }, LV_EVENT_FOCUSED, NULL);

    // Auto-fill password for known or saved networks
    String autofill_pass;
    bool has_saved_pw = !is_manual && is_saved_or_known_wifi(s_selected_ssid, autofill_pass);
    if (has_saved_pw) {
        lv_textarea_set_text(ta_wifi_pw, autofill_pass.c_str());
    }

    s_pw_visible = true;
    lv_textarea_set_password_mode(ta_wifi_pw, !s_pw_visible);
    lv_textarea_set_one_line(ta_wifi_pw, true);
    lv_obj_set_style_bg_color(ta_wifi_pw, lv_color_hex(0x13203C), 0);
    lv_obj_set_style_text_color(ta_wifi_pw, lv_color_hex(0x00E5FF), 0);

    // Eye toggle button
    lv_obj_t *btn_eye = lv_btn_create(wifi_pw_sheet);
    lv_obj_set_size(btn_eye, 44, 36);
    lv_obj_align(btn_eye, LV_ALIGN_TOP_RIGHT, -4, is_manual ? 64 : 56);
    lv_obj_set_style_bg_color(btn_eye, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_radius(btn_eye, 8, 0);
    lv_obj_t *lbl_eye = lv_label_create(btn_eye);
    lv_label_set_text(lbl_eye, LV_SYMBOL_EYE_OPEN);
    lv_obj_center(lbl_eye);
    lv_obj_add_event_cb(btn_eye, [](lv_event_t *e){
        s_pw_visible = !s_pw_visible;
        lv_textarea_set_password_mode(ta_wifi_pw, !s_pw_visible);
        lv_obj_t *l = (lv_obj_t *)lv_event_get_user_data(e);
        lv_label_set_text(l, s_pw_visible ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
    }, LV_EVENT_CLICKED, lbl_eye);

    // Status label
    lbl_wifi_pw_status = lv_label_create(wifi_pw_sheet);
    if (has_saved_pw) {
        lv_label_set_text(lbl_wifi_pw_status, "Mật khẩu đã được điền sẵn, bấm Kết nối!");
        lv_obj_set_style_text_color(lbl_wifi_pw_status, lv_color_hex(0x38BDF8), 0);
    } else {
        lv_label_set_text(lbl_wifi_pw_status, is_manual ? "Nhập tên mạng và mật khẩu" : "Nhập mật khẩu rồi bấm Kết nối");
        lv_obj_set_style_text_color(lbl_wifi_pw_status, lv_color_hex(0x94A3B8), 0);
    }
    lv_obj_align(lbl_wifi_pw_status, LV_ALIGN_TOP_MID, 0, is_manual ? 104 : 98);

    // Action buttons
    btn_wifi_pw_connect = lv_btn_create(wifi_pw_sheet);
    lv_obj_set_size(btn_wifi_pw_connect, 138, 34);
    lv_obj_align(btn_wifi_pw_connect, LV_ALIGN_TOP_LEFT, 4, is_manual ? 128 : 124);
    lv_obj_set_style_bg_color(btn_wifi_pw_connect, lv_color_hex(0x059669), 0); // Green
    lv_obj_set_style_radius(btn_wifi_pw_connect, 8, 0);
    lbl_wifi_pw_connect = lv_label_create(btn_wifi_pw_connect);
    lv_label_set_text(lbl_wifi_pw_connect, LV_SYMBOL_OK " Kết nối");
    lv_obj_center(lbl_wifi_pw_connect);
    lv_obj_add_event_cb(btn_wifi_pw_connect, [](lv_event_t *e){
        bool manual_mode = (ta_manual_ssid != nullptr);
        const char *target = manual_mode ? lv_textarea_get_text(ta_manual_ssid) : s_selected_ssid;
        const char *pw = lv_textarea_get_text(ta_wifi_pw);
        if (strlen(target) > 0) {
            start_wifi_connection_async(target, pw);
        }
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_cancel = lv_btn_create(wifi_pw_sheet);
    lv_obj_set_size(btn_cancel, 138, 34);
    lv_obj_align(btn_cancel, LV_ALIGN_TOP_RIGHT, -4, is_manual ? 128 : 124);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x334155), 0);
    lv_obj_set_style_radius(btn_cancel, 8, 0);
    lv_obj_t *lcan = lv_label_create(btn_cancel);
    lv_label_set_text(lcan, LV_SYMBOL_CLOSE " Hủy");
    lv_obj_center(lcan);
    lv_obj_add_event_cb(btn_cancel, [](lv_event_t *e){
        if (s_wifi_conn_timer) {
            lv_timer_del(s_wifi_conn_timer);
            s_wifi_conn_timer = nullptr;
            WiFi.disconnect();
        }
        if (wifi_pw_sheet) { lv_obj_del(wifi_pw_sheet); wifi_pw_sheet = nullptr; }
    }, LV_EVENT_CLICKED, NULL);

    // Keyboard
    kb_wifi_pw = lv_keyboard_create(wifi_pw_sheet);
    lv_obj_set_size(kb_wifi_pw, 292, 230);
    lv_obj_align(kb_wifi_pw, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_keyboard_set_textarea(kb_wifi_pw, is_manual ? ta_manual_ssid : ta_wifi_pw);
    lv_obj_add_event_cb(kb_wifi_pw, [](lv_event_t *e){
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_READY) {
            bool manual_mode = (ta_manual_ssid != nullptr);
            const char *target = manual_mode ? lv_textarea_get_text(ta_manual_ssid) : s_selected_ssid;
            const char *pw = lv_textarea_get_text(ta_wifi_pw);
            if (strlen(target) > 0) {
                start_wifi_connection_async(target, pw);
            }
        } else if (code == LV_EVENT_CANCEL) {
            if (wifi_pw_sheet) { lv_obj_del(wifi_pw_sheet); wifi_pw_sheet = nullptr; }
        }
    }, LV_EVENT_ALL, NULL);
}

/* --------------------------------------------------------------------
 * BẢN QUYỀN & MÀN HÌNH KHÓA (LICENSE GUARD MODAL & LOCK SCREEN)
 * -------------------------------------------------------------------- */
static lv_obj_t *s_license_modal = nullptr;
static lv_obj_t *ta_license_key = nullptr;
static lv_obj_t *lbl_license_status_text = nullptr;
static lv_obj_t *lbl_setting_license_sub = nullptr;

static void open_license_modal(bool is_lock_screen = false) {
    if (s_license_modal) return;
    ES8311_Audio::play_touch_sound(1200, 30);

    s_license_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_license_modal, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_pos(s_license_modal, 0, 0);
    lv_obj_set_style_bg_color(s_license_modal, lv_color_hex(0x060B18), 0);
    lv_obj_set_style_border_width(s_license_modal, 0, 0);
    lv_obj_set_style_pad_all(s_license_modal, 8, 0);
    lv_obj_clear_flag(s_license_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_obj_create(s_license_modal);
    lv_obj_set_size(hdr, 304, 96);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x0E172A), 0);
    lv_obj_set_style_border_color(hdr, lv_color_hex(is_lock_screen ? 0xEF4444 : 0x00E5FF), 0);
    lv_obj_set_style_border_width(hdr, 2, 0);
    lv_obj_set_style_radius(hdr, 12, 0);
    lv_obj_set_style_pad_all(hdr, 8, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_t = lv_label_create(hdr);
    lv_label_set_text(lbl_t, is_lock_screen ? LV_SYMBOL_WARNING " GIỚI HẠN DÙNG THỬ ĐÃ HẾT" : LV_SYMBOL_SETTINGS " BẢN QUYỀN THIẾT BỊ");
    lv_obj_set_style_text_color(lbl_t, lv_color_hex(is_lock_screen ? 0xEF4444 : 0x00E5FF), 0);
    lv_obj_set_style_text_font(lbl_t, &lv_font_vietnamese_14, 0);
    lv_obj_align(lbl_t, LV_ALIGN_TOP_LEFT, 2, 0);

    lv_obj_t *lbl_msg = lv_label_create(hdr);
    if (is_lock_screen) {
        lv_label_set_text(lbl_msg, "Mạch đã đạt giới hạn 1000 lần khởi động.\nVui lòng nhập mã key của tác giả để mở khóa vĩnh viễn.");
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "Số lần khởi động: %u / %u\nTrạng thái: %s\nNhập key tác giả để mở khóa vĩnh viễn:",
                 LicenseGuard::get_boot_count(), LicenseGuard::get_trial_limit(),
                 LicenseGuard::is_activated() ? "Đã kích hoạt vĩnh viễn" : "Bản dùng thử");
        lv_label_set_text(lbl_msg, buf);
    }
    lv_obj_set_style_text_color(lbl_msg, lv_color_hex(0xCBD5E1), 0);
    lv_obj_set_style_text_font(lbl_msg, &lv_font_vietnamese_11, 0);
    lv_obj_align(lbl_msg, LV_ALIGN_TOP_LEFT, 2, 24);

    ta_license_key = lv_textarea_create(s_license_modal);
    lv_obj_set_size(ta_license_key, 216, 36);
    lv_obj_align(ta_license_key, LV_ALIGN_TOP_LEFT, 8, 104);
    lv_textarea_set_placeholder_text(ta_license_key, "Nhập mã key...");
    lv_textarea_set_one_line(ta_license_key, true);
    lv_obj_set_style_bg_color(ta_license_key, lv_color_hex(0x13203C), 0);
    lv_obj_set_style_text_color(ta_license_key, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_color(ta_license_key, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_border_width(ta_license_key, 1, 0);
    lv_obj_set_style_radius(ta_license_key, 8, 0);

    lv_obj_t *btn_unlock = lv_btn_create(s_license_modal);
    lv_obj_set_size(btn_unlock, 82, 36);
    lv_obj_align(btn_unlock, LV_ALIGN_TOP_RIGHT, -8, 104);
    lv_obj_set_style_bg_color(btn_unlock, lv_color_hex(0x0284C7), 0);
    lv_obj_set_style_radius(btn_unlock, 8, 0);
    lv_obj_t *l_un = lv_label_create(btn_unlock);
    lv_label_set_text(l_un, "Mở khóa");
    lv_obj_set_style_text_font(l_un, &lv_font_vietnamese_11, 0);
    lv_obj_center(l_un);

    lbl_license_status_text = lv_label_create(s_license_modal);
    lv_label_set_text(lbl_license_status_text, "Nhập key rồi bấm Mở khóa hoặc phím (V)");
    lv_obj_set_style_text_color(lbl_license_status_text, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(lbl_license_status_text, &lv_font_vietnamese_11, 0);
    lv_obj_align(lbl_license_status_text, LV_ALIGN_TOP_LEFT, 10, 146);

    auto do_unlock = [](lv_event_t *e) {
        if (!ta_license_key) return;
        const char *txt = lv_textarea_get_text(ta_license_key);
        if (LicenseGuard::verify_and_activate(txt)) {
            ES8311_Audio::play_startup_chime();
            if (lbl_license_status_text) {
                lv_label_set_text(lbl_license_status_text, LV_SYMBOL_OK " KÍCH HOẠT THÀNH CÔNG VĨNH VIỄN!");
                lv_obj_set_style_text_color(lbl_license_status_text, lv_color_hex(0x10B981), 0);
            }
            if (lbl_setting_license_sub) {
                lv_label_set_text(lbl_setting_license_sub, LicenseGuard::get_license_status_string().c_str());
                lv_obj_set_style_text_color(lbl_setting_license_sub, lv_color_hex(0x10B981), 0);
            }
            lv_timer_t *t = lv_timer_create([](lv_timer_t *tm){
                if (s_license_modal) {
                    lv_obj_del(s_license_modal);
                    s_license_modal = nullptr;
                }
                lv_timer_del(tm);
            }, 1200, NULL);
        } else {
            ES8311_Audio::play_touch_sound(350, 120);
            if (lbl_license_status_text) {
                lv_label_set_text(lbl_license_status_text, LV_SYMBOL_CLOSE " Mã key không đúng! Vui lòng thử lại.");
                lv_obj_set_style_text_color(lbl_license_status_text, lv_color_hex(0xEF4444), 0);
            }
        }
    };

    lv_obj_add_event_cb(btn_unlock, do_unlock, LV_EVENT_CLICKED, NULL);

    if (!is_lock_screen) {
        lv_obj_t *btn_close = lv_btn_create(s_license_modal);
        lv_obj_set_size(btn_close, 304, 28);
        lv_obj_align(btn_close, LV_ALIGN_TOP_MID, 0, 168);
        lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x1E293B), 0);
        lv_obj_set_style_radius(btn_close, 8, 0);
        lv_obj_t *lc = lv_label_create(btn_close);
        lv_label_set_text(lc, LV_SYMBOL_CLOSE " Đóng");
        lv_obj_set_style_text_font(lc, &lv_font_vietnamese_11, 0);
        lv_obj_center(lc);
        lv_obj_add_event_cb(btn_close, [](lv_event_t *e){
            if (s_license_modal) {
                lv_obj_del(s_license_modal);
                s_license_modal = nullptr;
            }
        }, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *kb = lv_keyboard_create(s_license_modal);
    lv_obj_set_size(kb, 304, is_lock_screen ? 270 : 246);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_keyboard_set_textarea(kb, ta_license_key);
    lv_obj_add_event_cb(kb, do_unlock, LV_EVENT_READY, NULL);
}

static void close_wifi_manager_screen(lv_event_t *e) {
    if (s_wifi_scan_poll_timer) {
        lv_timer_del(s_wifi_scan_poll_timer);
        s_wifi_scan_poll_timer = nullptr;
    }
    if (s_wifi_scanning_active) {
        WiFi.scanDelete();
        s_wifi_scanning_active = false;
        WiFi.setAutoReconnect(true);
    }
    if (s_wifi_conn_timer) {
        lv_timer_del(s_wifi_conn_timer);
        s_wifi_conn_timer = nullptr;
    }
    if (wifi_pw_sheet) { lv_obj_del(wifi_pw_sheet); wifi_pw_sheet = nullptr; }
    if (wifi_full_screen) {
        lv_obj_del(wifi_full_screen);
        wifi_full_screen = nullptr;
        wifi_current_card = nullptr;
        lbl_wifi_curr_title = nullptr;
        lbl_wifi_curr_detail = nullptr;
        lbl_wifi_scan_count = nullptr;
        wifi_list_cont = nullptr;
        btn_wifi_rescan = nullptr;
        lbl_wifi_rescan = nullptr;
    }
    update_wifi_ui_status();
    ES8311_Audio::play_touch_sound(1000, 30);
}

static void open_wifi_manager_screen() {
    if (wifi_full_screen) return;
    ES8311_Audio::play_touch_sound(1200, 30);

    // Stop any background pending reconnection attempts so RF hardware is 100% free to scan
    if (WiFi.status() != WL_CONNECTED) {
        esp_wifi_disconnect();
        WiFi.disconnect(false);
    }

    wifi_full_screen = lv_obj_create(lv_scr_act());
    lv_obj_set_size(wifi_full_screen, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_pos(wifi_full_screen, 0, 0);
    lv_obj_set_style_bg_color(wifi_full_screen, lv_color_hex(0x060B18), 0);
    lv_obj_set_style_pad_all(wifi_full_screen, 0, 0);
    lv_obj_set_style_border_width(wifi_full_screen, 0, 0);
    lv_obj_set_style_radius(wifi_full_screen, 0, 0);
    lv_obj_clear_flag(wifi_full_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(wifi_full_screen, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // 1. Top Header Bar
    lv_obj_t *top_bar = lv_obj_create(wifi_full_screen);
    lv_obj_set_size(top_bar, SCREEN_WIDTH, 52);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(top_bar, lv_color_hex(0x0D1629), 0);
    lv_obj_set_style_border_side(top_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(top_bar, lv_color_hex(0x1E2E4E), 0);
    lv_obj_set_style_border_width(top_bar, 1, 0);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 6, 0);

    // Back button
    lv_obj_t *btn_back = lv_btn_create(top_bar);
    lv_obj_set_size(btn_back, 88, 38);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x13203C), 0);
    lv_obj_set_style_radius(btn_back, 8, 0);
    lv_obj_add_event_cb(btn_back, close_wifi_manager_screen, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_b = lv_label_create(btn_back);
    lv_label_set_text(lbl_b, LV_SYMBOL_LEFT " Cài đặt");
    lv_obj_set_style_text_color(lbl_b, lv_color_hex(0x00E5FF), 0);
    lv_obj_center(lbl_b);

    // Header Title
    lv_obj_t *lbl_title = lv_label_create(top_bar);
    lv_label_set_text(lbl_title, LV_SYMBOL_WIFI " MẠNG WI-FI");
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 8, 0);

    // Refresh Scan button
    btn_wifi_rescan = lv_btn_create(top_bar);
    lv_obj_set_size(btn_wifi_rescan, 82, 38);
    lv_obj_align(btn_wifi_rescan, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(btn_wifi_rescan, lv_color_hex(0x13203C), 0);
    lv_obj_set_style_border_color(btn_wifi_rescan, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(btn_wifi_rescan, 1, 0);
    lv_obj_set_style_radius(btn_wifi_rescan, 8, 0);
    lbl_wifi_rescan = lv_label_create(btn_wifi_rescan);
    lv_label_set_text(lbl_wifi_rescan, LV_SYMBOL_REFRESH " Quét");
    lv_obj_set_style_text_color(lbl_wifi_rescan, lv_color_hex(0x00E5FF), 0);
    lv_obj_center(lbl_wifi_rescan);
    lv_obj_add_event_cb(btn_wifi_rescan, [](lv_event_t *e){
        ES8311_Audio::play_touch_sound(1200, 30);
        scan_wifi_and_populate_list();
    }, LV_EVENT_CLICKED, NULL);

    // 2. Current Network Card
    wifi_current_card = lv_obj_create(wifi_full_screen);
    lv_obj_set_size(wifi_current_card, 304, 64);
    lv_obj_align(wifi_current_card, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(wifi_current_card, lv_color_hex(0x0D1D35), 0);
    lv_obj_set_style_border_width(wifi_current_card, 1, 0);
    lv_obj_set_style_radius(wifi_current_card, 12, 0);
    lv_obj_set_style_pad_all(wifi_current_card, 8, 0);
    lv_obj_clear_flag(wifi_current_card, LV_OBJ_FLAG_SCROLLABLE);

    lbl_wifi_curr_title = lv_label_create(wifi_current_card);
    lv_obj_align(lbl_wifi_curr_title, LV_ALIGN_TOP_LEFT, 6, 2);

    lbl_wifi_curr_detail = lv_label_create(wifi_current_card);
    lv_obj_align(lbl_wifi_curr_detail, LV_ALIGN_BOTTOM_LEFT, 6, -2);

    update_current_wifi_card_ui();

    lv_obj_add_event_cb(wifi_current_card, [](lv_event_t *e){
        if (WiFi.status() == WL_CONNECTED) {
            ES8311_Audio::play_touch_sound(1200, 30);
            open_connected_network_dialog(cur_wifi_ssid.c_str(), WiFi.RSSI());
        } else {
            ES8311_Audio::play_touch_sound(1000, 20);
            scan_wifi_and_populate_list();
        }
    }, LV_EVENT_CLICKED, NULL);

    // 3. Section Title
    lbl_wifi_scan_count = lv_label_create(wifi_full_screen);
    lv_label_set_text(lbl_wifi_scan_count, "MẠNG KHẢ DỤNG");
    lv_obj_set_style_text_color(lbl_wifi_scan_count, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(lbl_wifi_scan_count, LV_ALIGN_TOP_LEFT, 12, 134);

    // 4. Scrollable Networks List Container
    wifi_list_cont = lv_obj_create(wifi_full_screen);
    lv_obj_set_size(wifi_list_cont, 308, 316);
    lv_obj_align(wifi_list_cont, LV_ALIGN_TOP_MID, 0, 156);
    lv_obj_set_style_bg_color(wifi_list_cont, lv_color_hex(0x060B18), 0);
    lv_obj_set_style_border_width(wifi_list_cont, 0, 0);
    lv_obj_set_style_pad_all(wifi_list_cont, 0, 0);
    lv_obj_set_flex_flow(wifi_list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wifi_list_cont, 8, 0);
    lv_obj_set_scroll_dir(wifi_list_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(wifi_list_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(wifi_list_cont, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // Pre-populate known networks immediately if list is empty so user never sees blank screen
    if (s_scanned_count == 0) {
        reload_nvs_wifi_cache();
        for (size_t i = 0; i < sizeof(s_known_wifis) / sizeof(s_known_wifis[0]) && s_scanned_count < 30; i++) {
            if (!s_known_wifis[i].ssid || strlen(s_known_wifis[i].ssid) == 0) continue;
            strncpy(s_scanned_wifis[s_scanned_count].ssid, s_known_wifis[i].ssid, sizeof(s_scanned_wifis[0].ssid) - 1);
            s_scanned_wifis[s_scanned_count].rssi = -60;
            s_scanned_wifis[s_scanned_count].is_open = false;
            s_scanned_count++;
        }
    }

    // Render list immediately (showing known networks + manual entry button)
    populate_wifi_list_ui();

    // Trigger initial async scan
    scan_wifi_and_populate_list();
}

/* ====================================================================
 * BUILD REDESIGNED MINIMALIST LUXURY UI (PURE VIETNAMESE TYPOGRAPHY)
 * ==================================================================== */
static lv_obj_t *create_screen_header(lv_obj_t *parent, const char *center_title, lv_obj_t **lbl_time_out) {
    lv_obj_t *hdr = lv_obj_create(parent);
    lv_obj_set_size(hdr, 296, 32);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    // Left: MINIMAZING logo (synchronized across all 4 pages)
    lv_obj_t *lbl_b = lv_label_create(hdr);
    lv_label_set_text(lbl_b, "MINIMAZING");
    lv_obj_set_style_text_color(lbl_b, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_b, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_b, LV_ALIGN_LEFT_MID, 0, 0);

    // Center Title (if any)
    if (center_title && strlen(center_title) > 0) {
        lv_obj_t *lbl_c = lv_label_create(hdr);
        lv_label_set_text(lbl_c, center_title);
        lv_obj_set_style_text_color(lbl_c, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_text_font(lbl_c, &lv_font_vietnamese_14, 0);
        lv_obj_align(lbl_c, LV_ALIGN_CENTER, 0, 0);
    }

    // Right: Wi-Fi, Time and Date (stacked right-aligned)
    *lbl_time_out = lv_label_create(hdr);
    lv_label_set_text(*lbl_time_out, LV_SYMBOL_WIFI "  10:30\n08/09/2026");
    lv_obj_set_style_text_color(*lbl_time_out, lv_color_hex(0xF8FAFC), 0);
    lv_obj_set_style_text_font(*lbl_time_out, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_align(*lbl_time_out, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_line_space(*lbl_time_out, 2, 0);
    lv_obj_align(*lbl_time_out, LV_ALIGN_RIGHT_MID, 0, 0);

    return hdr;
}

void build_smart_fridge_ui() {
    tv = lv_tabview_create(lv_scr_act(), LV_DIR_BOTTOM, 0);
    lv_obj_set_style_bg_color(tv, lv_color_hex(0x060B18), 0);
    lv_obj_set_style_pad_all(tv, 0, 0);

    // Synchronize footer navbar icons whenever tabs are switched (via swipe or tap)
    lv_obj_add_event_cb(tv, [](lv_event_t *e){
        lv_obj_t *tabview = lv_event_get_target(e);
        uint16_t act_tab = lv_tabview_get_tab_act(tabview);
        update_nav_highlight(act_tab);
    }, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *t1 = lv_tabview_add_tab(tv, "Trang chủ");
    lv_obj_t *t2 = lv_tabview_add_tab(tv, "Ngăn lạnh");
    lv_obj_t *t3 = lv_tabview_add_tab(tv, "Trợ lý");
    lv_obj_t *t4 = lv_tabview_add_tab(tv, "Cài đặt");

    lv_obj_add_event_cb(lv_scr_act(), screen_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_GESTURE_BUBBLE);

    for (lv_obj_t *t : {t1, t2, t3, t4}) {
        lv_obj_set_style_bg_color(t, lv_color_hex(0x060B18), 0);
        lv_obj_set_style_pad_all(t, 12, 0);
        lv_obj_add_event_cb(t, screen_gesture_cb, LV_EVENT_GESTURE, NULL);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* ----------------------------------------------------------------
     * TAB 1: TRANG CHỦ (EXACT FIGMA REPLICA)
     * ---------------------------------------------------------------- */
    // 1. Top Header
    create_screen_header(t1, NULL, &lbl_time1);

    // 2. Hero Showcase Card (y = 32, h = 180)
    lv_obj_t *hero_card = lv_obj_create(t1);
    lv_obj_set_size(hero_card, 296, 180);
    lv_obj_align(hero_card, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_bg_color(hero_card, lv_color_hex(0x08101E), 0);
    lv_obj_set_style_border_color(hero_card, lv_color_hex(0x15263F), 0);
    lv_obj_set_style_border_width(hero_card, 1, 0);
    lv_obj_set_style_radius(hero_card, 16, 0);
    lv_obj_set_style_pad_all(hero_card, 0, 0);
    lv_obj_clear_flag(hero_card, LV_OBJ_FLAG_SCROLLABLE);

    // Center Refrigerator & Pedestal Glow
    lv_obj_t *fridge_pedestal = lv_obj_create(hero_card);
    lv_obj_set_size(fridge_pedestal, 68, 6);
    lv_obj_align(fridge_pedestal, LV_ALIGN_TOP_MID, 0, 136);
    lv_obj_set_style_bg_color(fridge_pedestal, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_bg_opa(fridge_pedestal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(fridge_pedestal, 0, 0);
    lv_obj_set_style_radius(fridge_pedestal, 3, 0);
    lv_obj_clear_flag(fridge_pedestal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *fridge_img_obj = lv_img_create(hero_card);
    lv_img_set_src(fridge_img_obj, &img_fridge);
    lv_obj_align(fridge_img_obj, LV_ALIGN_TOP_MID, 0, 2);

    // Left Zone: NGĂN MÁT
    lv_obj_t *l_mat_hdr = lv_label_create(hero_card);
    lv_label_set_text(l_mat_hdr, "NGĂN MÁT");
    lv_obj_set_style_text_color(l_mat_hdr, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(l_mat_hdr, &lv_font_vietnamese_11, 0);
    lv_obj_align(l_mat_hdr, LV_ALIGN_TOP_LEFT, 16, 12);

    lbl_home_mat = lv_label_create(hero_card);
    lv_label_set_text(lbl_home_mat, "4");
    lv_obj_set_style_text_font(lbl_home_mat, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(lbl_home_mat, lv_color_hex(0x00E5FF), 0);
    lv_obj_align(lbl_home_mat, LV_ALIGN_TOP_LEFT, 16, 28);

    lv_obj_t *lbl_mat_unit = lv_label_create(hero_card);
    lv_label_set_text(lbl_mat_unit, "°C");
    lv_obj_set_style_text_font(lbl_mat_unit, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_mat_unit, lv_color_hex(0x94A3B8), 0);
    lv_obj_align_to(lbl_mat_unit, lbl_home_mat, LV_ALIGN_OUT_RIGHT_MID, 4, 4);

    lv_obj_t *btn_snow_mat = lv_btn_create(hero_card);
    lv_obj_set_size(btn_snow_mat, 36, 36);
    lv_obj_align(btn_snow_mat, LV_ALIGN_TOP_LEFT, 16, 82);
    lv_obj_set_style_radius(btn_snow_mat, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn_snow_mat, lv_color_hex(0x0E1C33), 0);
    lv_obj_set_style_border_color(btn_snow_mat, lv_color_hex(0x1E3A5F), 0);
    lv_obj_set_style_border_width(btn_snow_mat, 1, 0);
    lv_obj_set_style_pad_all(btn_snow_mat, 0, 0);
    lv_obj_t *ic_snow1 = lv_img_create(btn_snow_mat);
    lv_img_set_src(ic_snow1, &icon_snowflake_blue);
    lv_obj_center(ic_snow1);
    lv_obj_add_event_cb(btn_snow_mat, [](lv_event_t *e){
        lv_tabview_set_act(tv, 1, LV_ANIM_ON);
        update_nav_highlight(1);
        ES8311_Audio::play_touch_sound(1200, 30);
    }, LV_EVENT_CLICKED, NULL);

    // Right Zone: NGĂN ĐÔNG
    lv_obj_t *l_dong_hdr = lv_label_create(hero_card);
    lv_label_set_text(l_dong_hdr, "NGĂN ĐÔNG");
    lv_obj_set_style_text_color(l_dong_hdr, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(l_dong_hdr, &lv_font_vietnamese_11, 0);
    lv_obj_align(l_dong_hdr, LV_ALIGN_TOP_RIGHT, -16, 12);

    lbl_home_dong = lv_label_create(hero_card);
    lv_label_set_text(lbl_home_dong, "-18");
    lv_obj_set_style_text_font(lbl_home_dong, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(lbl_home_dong, lv_color_hex(0x00E5FF), 0);
    lv_obj_align(lbl_home_dong, LV_ALIGN_TOP_RIGHT, -42, 28);

    lv_obj_t *lbl_dong_unit = lv_label_create(hero_card);
    lv_label_set_text(lbl_dong_unit, "°C");
    lv_obj_set_style_text_font(lbl_dong_unit, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_dong_unit, lv_color_hex(0x94A3B8), 0);
    lv_obj_align_to(lbl_dong_unit, lbl_home_dong, LV_ALIGN_OUT_RIGHT_MID, 4, 4);

    lv_obj_t *btn_snow_dong = lv_btn_create(hero_card);
    lv_obj_set_size(btn_snow_dong, 36, 36);
    lv_obj_align(btn_snow_dong, LV_ALIGN_TOP_RIGHT, -16, 82);
    lv_obj_set_style_radius(btn_snow_dong, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn_snow_dong, lv_color_hex(0x0E1C33), 0);
    lv_obj_set_style_border_color(btn_snow_dong, lv_color_hex(0x1E3A5F), 0);
    lv_obj_set_style_border_width(btn_snow_dong, 1, 0);
    lv_obj_set_style_pad_all(btn_snow_dong, 0, 0);
    lv_obj_t *ic_snow2 = lv_img_create(btn_snow_dong);
    lv_img_set_src(ic_snow2, &icon_snowflake_blue);
    lv_obj_center(ic_snow2);
    lv_obj_add_event_cb(btn_snow_dong, [](lv_event_t *e){
        lv_tabview_set_act(tv, 1, LV_ANIM_ON);
        update_nav_highlight(1);
        ES8311_Audio::play_touch_sound(1200, 30);
    }, LV_EVENT_CLICKED, NULL);

    // Bottom Status Bar
    lv_obj_t *status_strip = lv_obj_create(hero_card);
    lv_obj_set_size(status_strip, 296, 32);
    lv_obj_align(status_strip, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(status_strip, lv_color_hex(0x060D19), 0);
    lv_obj_set_style_border_side(status_strip, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(status_strip, lv_color_hex(0x15263F), 0);
    lv_obj_set_style_border_width(status_strip, 1, 0);
    lv_obj_set_style_radius(status_strip, 0, 0);
    lv_obj_set_style_pad_all(status_strip, 0, 0);
    lv_obj_clear_flag(status_strip, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l_st_lbl = lv_label_create(status_strip);
    lv_label_set_text(l_st_lbl, "Trạng thái");
    lv_obj_set_style_text_color(l_st_lbl, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(l_st_lbl, &lv_font_vietnamese_11, 0);
    lv_obj_align(l_st_lbl, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *ic_shield = lv_img_create(status_strip);
    lv_img_set_src(ic_shield, &icon_shield_green);
    lv_obj_align(ic_shield, LV_ALIGN_LEFT_MID, 90, 0);

    lv_obj_t *l_st_val = lv_label_create(status_strip);
    lv_label_set_text(l_st_val, "Hoạt động bình thường");
    lv_obj_set_style_text_color(l_st_val, lv_color_hex(0x22C55E), 0);
    lv_obj_set_style_text_font(l_st_val, &lv_font_vietnamese_11, 0);
    lv_obj_align(l_st_val, LV_ALIGN_LEFT_MID, 116, 0);

    lv_obj_t *l_st_arr = lv_label_create(status_strip);
    lv_label_set_text(l_st_arr, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(l_st_arr, lv_color_hex(0x64748B), 0);
    lv_obj_align(l_st_arr, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(status_strip, [](lv_event_t *e){
        lv_tabview_set_act(tv, 1, LV_ANIM_ON);
        update_nav_highlight(1);
        ES8311_Audio::play_touch_sound(1200, 30);
    }, LV_EVENT_CLICKED, NULL);

    // 3. Middle Metric Row (4 Cards) (y = 218, h = 68)
    struct MetricDef {
        const lv_img_dsc_t *icon;
        const char *title;
        const char *val;
        uint32_t color;
    } metrics[4] = {
        {&icon_leaf_green, "TIẾT KIỆM ĐIỆN", "Đang bật", 0x22C55E},
        {&icon_droplet_blue, "ĐỘ ẨM", "45%", 0x38BDF8},
        {&icon_filter_cyan, "ĐỘ LỌC", "Tốt", 0x38BDF8},
        {&icon_lock_amber, "KHÓA TRẺ EM", "Tắt", 0x64748B},
    };

    for (int i = 0; i < 4; i++) {
        lv_obj_t *mc = lv_obj_create(t1);
        lv_obj_set_size(mc, 69, 68);
        lv_obj_align(mc, LV_ALIGN_TOP_LEFT, i * 75, 218);
        lv_obj_set_style_bg_color(mc, lv_color_hex(0x08101E), 0);
        lv_obj_set_style_border_color(mc, lv_color_hex(0x15263F), 0);
        lv_obj_set_style_border_width(mc, 1, 0);
        lv_obj_set_style_radius(mc, 10, 0);
        lv_obj_set_style_pad_all(mc, 3, 0);
        lv_obj_clear_flag(mc, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *ic = lv_img_create(mc);
        lv_img_set_src(ic, metrics[i].icon);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 2);

        lv_obj_t *lt = lv_label_create(mc);
        lv_label_set_text(lt, metrics[i].title);
        lv_obj_set_style_text_color(lt, lv_color_hex(0x94A3B8), 0);
        lv_obj_set_style_text_font(lt, &lv_font_vietnamese_11, 0);
        lv_obj_set_style_text_align(lt, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lt, LV_ALIGN_TOP_MID, 0, 26);

        lv_obj_t *lv = lv_label_create(mc);
        lv_label_set_text(lv, metrics[i].val);
        lv_obj_set_style_text_color(lv, lv_color_hex(metrics[i].color), 0);
        lv_obj_set_style_text_font(lv, &lv_font_vietnamese_11, 0);
        lv_obj_set_style_text_align(lv, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lv, LV_ALIGN_TOP_MID, 0, 46);

        if (i == 0) lbl_home_eco_state = lv;
        if (i == 3) lbl_home_lock_state = lv;

        // Interactive tap on Eco or Child Lock
        if (i == 0) {
            lv_obj_add_event_cb(mc, [](lv_event_t *e){
                s_eco_mode = !s_eco_mode;
                if (lbl_home_eco_state) {
                    lv_label_set_text(lbl_home_eco_state, s_eco_mode ? "Đang bật" : "Tắt");
                    lv_obj_set_style_text_color(lbl_home_eco_state, lv_color_hex(s_eco_mode ? 0x22C55E : 0x64748B), 0);
                }
                if (sw_settings_eco) {
                    if (s_eco_mode) lv_obj_add_state(sw_settings_eco, LV_STATE_CHECKED);
                    else lv_obj_clear_state(sw_settings_eco, LV_STATE_CHECKED);
                }
                ES8311_Audio::play_touch_sound(s_eco_mode ? 1400 : 900, 30);
            }, LV_EVENT_CLICKED, NULL);
        } else if (i == 3) {
            lv_obj_add_event_cb(mc, [](lv_event_t *e){
                s_child_lock = !s_child_lock;
                if (lbl_home_lock_state) {
                    lv_label_set_text(lbl_home_lock_state, s_child_lock ? "Đang bật" : "Tắt");
                    lv_obj_set_style_text_color(lbl_home_lock_state, lv_color_hex(s_child_lock ? 0xF59E0B : 0x64748B), 0);
                }
                if (sw_settings_lock) {
                    if (s_child_lock) lv_obj_add_state(sw_settings_lock, LV_STATE_CHECKED);
                    else lv_obj_clear_state(sw_settings_lock, LV_STATE_CHECKED);
                }
                ES8311_Audio::play_touch_sound(s_child_lock ? 1200 : 800, 30);
            }, LV_EVENT_CLICKED, NULL);
        }
    }

    // 4. Quick Features Section (y = 292, h = 94)
    lv_obj_t *lbl_q_hdr = lv_label_create(t1);
    lv_label_set_text(lbl_q_hdr, "TÍNH NĂNG NHANH");
    lv_obj_set_style_text_color(lbl_q_hdr, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(lbl_q_hdr, &lv_font_vietnamese_11, 0);
    lv_obj_align(lbl_q_hdr, LV_ALIGN_TOP_LEFT, 0, 292);

    struct QuickActionDef {
        const lv_img_dsc_t *icon;
        const char *name;
    } q_actions[4] = {
        {&icon_snowflake_blue, "Làm nhanh"},
        {&icon_ice_bucket, "Làm đá nhanh"},
        {&icon_leaf_green, "Tiết kiệm"},
        {&icon_bell_yellow, "Thông báo"},
    };

    for (int i = 0; i < 4; i++) {
        lv_obj_t *qb = lv_btn_create(t1);
        lv_obj_set_size(qb, 69, 70);
        lv_obj_align(qb, LV_ALIGN_TOP_LEFT, i * 75, 312);
        lv_obj_set_style_bg_color(qb, lv_color_hex(0x08101E), 0);
        lv_obj_set_style_bg_color(qb, lv_color_hex(0x13233D), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(qb, lv_color_hex(0x15263F), 0);
        lv_obj_set_style_border_width(qb, 1, 0);
        lv_obj_set_style_radius(qb, 10, 0);
        lv_obj_set_style_pad_all(qb, 3, 0);

        lv_obj_t *ic = lv_img_create(qb);
        lv_img_set_src(ic, q_actions[i].icon);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 6);

        lv_obj_t *lt = lv_label_create(qb);
        lv_label_set_text(lt, q_actions[i].name);
        lv_obj_set_style_text_color(lt, lv_color_hex(0xF8FAFC), 0);
        lv_obj_set_style_text_font(lt, &lv_font_vietnamese_11, 0);
        lv_obj_set_style_text_align(lt, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lt, LV_ALIGN_BOTTOM_MID, 0, -4);

        lv_obj_add_event_cb(qb, [](lv_event_t *e){
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            ES8311_Audio::play_touch_sound(1200 + idx * 100, 30);
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    // 5. Pagination Dots (y = 392)
    lv_obj_t *p_dot1 = lv_obj_create(t1);
    lv_obj_set_size(p_dot1, 16, 5);
    lv_obj_align(p_dot1, LV_ALIGN_TOP_MID, -14, 392);
    lv_obj_set_style_bg_color(p_dot1, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(p_dot1, 0, 0);
    lv_obj_set_style_radius(p_dot1, 3, 0);

    lv_obj_t *p_dot2 = lv_obj_create(t1);
    lv_obj_set_size(p_dot2, 5, 5);
    lv_obj_align(p_dot2, LV_ALIGN_TOP_MID, 0, 392);
    lv_obj_set_style_bg_color(p_dot2, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(p_dot2, 0, 0);
    lv_obj_set_style_radius(p_dot2, 3, 0);

    lv_obj_t *p_dot3 = lv_obj_create(t1);
    lv_obj_set_size(p_dot3, 5, 5);
    lv_obj_align(p_dot3, LV_ALIGN_TOP_MID, 12, 392);
    lv_obj_set_style_bg_color(p_dot3, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(p_dot3, 0, 0);
    lv_obj_set_style_radius(p_dot3, 3, 0);

    /* ----------------------------------------------------------------
     * TAB 2: NGĂN LẠNH (EXACT FIGMA REPLICA)
     * ---------------------------------------------------------------- */
    // 1. Header
    create_screen_header(t2, "NGĂN LẠNH", &lbl_time2);

    // 2. Showcase Top Card (y = 32, h = 138)
    lv_obj_t *card_m_show = lv_obj_create(t2);
    lv_obj_set_size(card_m_show, 296, 138);
    lv_obj_align(card_m_show, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_bg_color(card_m_show, lv_color_hex(0x08101E), 0);
    lv_obj_set_style_border_color(card_m_show, lv_color_hex(0x15263F), 0);
    lv_obj_set_style_border_width(card_m_show, 1, 0);
    lv_obj_set_style_radius(card_m_show, 14, 0);
    lv_obj_set_style_pad_all(card_m_show, 4, 0);
    lv_obj_clear_flag(card_m_show, LV_OBJ_FLAG_SCROLLABLE);

    // Inside Refrigerator Frame
    lv_obj_t *fridge_inside_frame = lv_obj_create(card_m_show);
    lv_obj_set_size(fridge_inside_frame, 114, 128);
    lv_obj_align(fridge_inside_frame, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(fridge_inside_frame, lv_color_hex(0x0A182E), 0);
    lv_obj_set_style_border_color(fridge_inside_frame, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(fridge_inside_frame, 1, 0);
    lv_obj_set_style_radius(fridge_inside_frame, 10, 0);
    lv_obj_set_style_pad_all(fridge_inside_frame, 2, 0);
    lv_obj_clear_flag(fridge_inside_frame, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img_in = lv_img_create(fridge_inside_frame);
    lv_img_set_src(img_in, &img_fridge_inside);
    lv_obj_center(img_in);

    // Right Side: Temperature & Quick Cool
    lv_obj_t *l_curr_t_hdr = lv_label_create(card_m_show);
    lv_label_set_text(l_curr_t_hdr, "NHIỆT ĐỘ HIỆN TẠI");
    lv_obj_set_style_text_color(l_curr_t_hdr, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(l_curr_t_hdr, &lv_font_vietnamese_11, 0);
    lv_obj_align(l_curr_t_hdr, LV_ALIGN_TOP_RIGHT, -14, 10);

    lbl_tab2_mat = lv_label_create(card_m_show);
    char init_t_buf[16];
    snprintf(init_t_buf, sizeof(init_t_buf), "%d", temp_mat);
    lv_label_set_text(lbl_tab2_mat, init_t_buf);
    lv_obj_set_style_text_font(lbl_tab2_mat, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(lbl_tab2_mat, lv_color_hex(0x00E5FF), 0);
    lv_obj_align(lbl_tab2_mat, LV_ALIGN_TOP_RIGHT, -64, 28);

    lv_obj_t *l_curr_t_unit = lv_label_create(card_m_show);
    lv_label_set_text(l_curr_t_unit, "°C");
    lv_obj_set_style_text_font(l_curr_t_unit, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l_curr_t_unit, lv_color_hex(0xF8FAFC), 0);
    lv_obj_align_to(l_curr_t_unit, lbl_tab2_mat, LV_ALIGN_OUT_RIGHT_MID, 4, 4);

    lv_obj_t *btn_cool_circle = lv_btn_create(card_m_show);
    lv_obj_set_size(btn_cool_circle, 40, 40);
    lv_obj_align(btn_cool_circle, LV_ALIGN_TOP_RIGHT, -54, 72);
    lv_obj_set_style_radius(btn_cool_circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn_cool_circle, lv_color_hex(0x0E1C33), 0);
    lv_obj_set_style_border_color(btn_cool_circle, lv_color_hex(0x1E3A5F), 0);
    lv_obj_set_style_border_width(btn_cool_circle, 1, 0);
    lv_obj_set_style_pad_all(btn_cool_circle, 0, 0);
    lv_obj_t *ic_sc = lv_img_create(btn_cool_circle);
    lv_img_set_src(ic_sc, &icon_snowflake_blue);
    lv_obj_center(ic_sc);

    lv_obj_t *l_cool_lbl = lv_label_create(card_m_show);
    lv_label_set_text(l_cool_lbl, "Làm lạnh");
    lv_obj_set_style_text_color(l_cool_lbl, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(l_cool_lbl, &lv_font_vietnamese_11, 0);
    lv_obj_align(l_cool_lbl, LV_ALIGN_BOTTOM_RIGHT, -48, -6);

    // 3. Temperature Adjustment Card (y = 176, h = 80)
    lv_obj_t *card_m_adj = lv_obj_create(t2);
    lv_obj_set_size(card_m_adj, 296, 80);
    lv_obj_align(card_m_adj, LV_ALIGN_TOP_MID, 0, 176);
    lv_obj_set_style_bg_color(card_m_adj, lv_color_hex(0x08101E), 0);
    lv_obj_set_style_border_color(card_m_adj, lv_color_hex(0x15263F), 0);
    lv_obj_set_style_border_width(card_m_adj, 1, 0);
    lv_obj_set_style_radius(card_m_adj, 14, 0);
    lv_obj_set_style_pad_all(card_m_adj, 6, 0);
    lv_obj_clear_flag(card_m_adj, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l_adj_hdr = lv_label_create(card_m_adj);
    lv_label_set_text(l_adj_hdr, "ĐIỀU CHỈNH NHIỆT ĐỘ");
    lv_obj_set_style_text_color(l_adj_hdr, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(l_adj_hdr, &lv_font_vietnamese_11, 0);
    lv_obj_align(l_adj_hdr, LV_ALIGN_TOP_MID, 0, 2);

    lbl_ctrl_mat = lv_label_create(card_m_adj);
    lv_label_set_text(lbl_ctrl_mat, "4°C");
    lv_obj_set_style_text_font(lbl_ctrl_mat, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_ctrl_mat, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_ctrl_mat, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *bm_minus = lv_btn_create(card_m_adj);
    lv_obj_set_size(bm_minus, 32, 32);
    lv_obj_align(bm_minus, LV_ALIGN_LEFT_MID, 10, 10);
    lv_obj_set_style_radius(bm_minus, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bm_minus, lv_color_hex(0x0E1C33), 0);
    lv_obj_set_style_border_color(bm_minus, lv_color_hex(0x1E3A5F), 0);
    lv_obj_set_style_border_width(bm_minus, 1, 0);
    lv_obj_t *lbl_min = lv_label_create(bm_minus);
    lv_label_set_text(lbl_min, "-");
    lv_obj_set_style_text_font(lbl_min, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl_min, lv_color_hex(0x00E5FF), 0);
    lv_obj_center(lbl_min);
    lv_obj_add_event_cb(bm_minus, btn_mat_minus_event, LV_EVENT_CLICKED, NULL);

    slider_mat = lv_slider_create(card_m_adj);
    lv_slider_set_range(slider_mat, 0, 10);
    lv_slider_set_value(slider_mat, temp_mat, LV_ANIM_OFF);
    lv_obj_set_size(slider_mat, 150, 8);
    lv_obj_align(slider_mat, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(slider_mat, lv_color_hex(0x1E2E4E), 0);
    lv_obj_set_style_bg_color(slider_mat, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_mat, lv_color_hex(0x00E5FF), LV_PART_KNOB);
    lv_obj_add_event_cb(slider_mat, slider_mat_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *bm_plus = lv_btn_create(card_m_adj);
    lv_obj_set_size(bm_plus, 32, 32);
    lv_obj_align(bm_plus, LV_ALIGN_RIGHT_MID, -10, 10);
    lv_obj_set_style_radius(bm_plus, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bm_plus, lv_color_hex(0x0E1C33), 0);
    lv_obj_set_style_border_color(bm_plus, lv_color_hex(0x1E3A5F), 0);
    lv_obj_set_style_border_width(bm_plus, 1, 0);
    lv_obj_t *lbl_pls = lv_label_create(bm_plus);
    lv_label_set_text(lbl_pls, "+");
    lv_obj_set_style_text_font(lbl_pls, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl_pls, lv_color_hex(0x00E5FF), 0);
    lv_obj_center(lbl_pls);
    lv_obj_add_event_cb(bm_plus, btn_mat_plus_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l_tick0 = lv_label_create(card_m_adj);
    lv_label_set_text(l_tick0, "0°C");
    lv_obj_set_style_text_color(l_tick0, lv_color_hex(0x64748B), 0);
    lv_obj_set_style_text_font(l_tick0, &lv_font_montserrat_10, 0);
    lv_obj_align(l_tick0, LV_ALIGN_BOTTOM_LEFT, 52, -2);

    lv_obj_t *l_tick10 = lv_label_create(card_m_adj);
    lv_label_set_text(l_tick10, "10°C");
    lv_obj_set_style_text_color(l_tick10, lv_color_hex(0x64748B), 0);
    lv_obj_set_style_text_font(l_tick10, &lv_font_montserrat_10, 0);
    lv_obj_align(l_tick10, LV_ALIGN_BOTTOM_RIGHT, -52, -2);

    // 4. Cooling Mode Section (y = 262, h = 74)
    lv_obj_t *l_cm_hdr = lv_label_create(t2);
    lv_label_set_text(l_cm_hdr, "CHẾ ĐỘ LÀM LẠNH");
    lv_obj_set_style_text_color(l_cm_hdr, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(l_cm_hdr, &lv_font_vietnamese_11, 0);
    lv_obj_align(l_cm_hdr, LV_ALIGN_TOP_LEFT, 0, 262);

    struct CoolModeDef {
        const lv_img_dsc_t *icon;
        const char *title;
    } c_modes[3] = {
        {&icon_snowflake_blue, "Làm lạnh nhanh"},
        {&icon_leaf_green, "Tiết kiệm điện"},
        {&icon_custom_mode, "Tùy chỉnh"},
    };

    for (int i = 0; i < 3; i++) {
        card_modes[i] = lv_obj_create(t2);
        lv_obj_set_size(card_modes[i], 94, 56);
        lv_obj_align(card_modes[i], LV_ALIGN_TOP_LEFT, i * 101, 278);
        bool is_act = (i == s_cooling_mode);
        lv_obj_set_style_bg_color(card_modes[i], is_act ? lv_color_hex(0x0C1C36) : lv_color_hex(0x08101E), 0);
        lv_obj_set_style_border_color(card_modes[i], is_act ? lv_color_hex(0x00E5FF) : lv_color_hex(0x15263F), 0);
        lv_obj_set_style_border_width(card_modes[i], is_act ? 2 : 1, 0);
        lv_obj_set_style_radius(card_modes[i], 10, 0);
        lv_obj_set_style_pad_all(card_modes[i], 4, 0);
        lv_obj_clear_flag(card_modes[i], LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *ic = lv_img_create(card_modes[i]);
        lv_img_set_src(ic, c_modes[i].icon);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 2);

        lv_obj_t *lt = lv_label_create(card_modes[i]);
        lv_label_set_text(lt, c_modes[i].title);
        lv_obj_set_style_text_color(lt, is_act ? lv_color_hex(0x00E5FF) : lv_color_hex(0x94A3B8), 0);
        lv_obj_set_style_text_font(lt, &lv_font_vietnamese_11, 0);
        lv_obj_set_style_text_align(lt, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lt, LV_ALIGN_BOTTOM_MID, 0, -2);

        lv_obj_add_event_cb(card_modes[i], [](lv_event_t *e){
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            s_cooling_mode = idx;
            for (int k = 0; k < 3; k++) {
                if (card_modes[k]) {
                    bool act = (k == s_cooling_mode);
                    lv_obj_set_style_bg_color(card_modes[k], act ? lv_color_hex(0x0C1C36) : lv_color_hex(0x08101E), 0);
                    lv_obj_set_style_border_color(card_modes[k], act ? lv_color_hex(0x00E5FF) : lv_color_hex(0x15263F), 0);
                    lv_obj_set_style_border_width(card_modes[k], act ? 2 : 1, 0);
                    lv_obj_t *lbl = lv_obj_get_child(card_modes[k], 1);
                    if (lbl) lv_obj_set_style_text_color(lbl, act ? lv_color_hex(0x00E5FF) : lv_color_hex(0x94A3B8), 0);
                }
            }
            ES8311_Audio::play_touch_sound(1200 + idx * 100, 30);
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    // 5. Camera Tap Card (y = 342, h = 60)
    lv_obj_t *card_cam = lv_obj_create(t2);
    lv_obj_set_size(card_cam, 296, 60);
    lv_obj_align(card_cam, LV_ALIGN_TOP_MID, 0, 342);
    lv_obj_set_style_bg_color(card_cam, lv_color_hex(0x08101E), 0);
    lv_obj_set_style_border_color(card_cam, lv_color_hex(0x15263F), 0);
    lv_obj_set_style_border_width(card_cam, 1, 0);
    lv_obj_set_style_radius(card_cam, 12, 0);
    lv_obj_set_style_pad_all(card_cam, 6, 0);
    lv_obj_clear_flag(card_cam, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic_cam = lv_img_create(card_cam);
    lv_img_set_src(ic_cam, &icon_camera_cyan);
    lv_obj_align(ic_cam, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *l_cam_h = lv_label_create(card_cam);
    lv_label_set_text(l_cam_h, "GÕ 2 LẦN ĐỂ XEM CAMERA");
    lv_obj_set_style_text_color(l_cam_h, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l_cam_h, &lv_font_vietnamese_11, 0);
    lv_obj_align(l_cam_h, LV_ALIGN_TOP_LEFT, 54, 4);

    lv_obj_t *l_cam_sub = lv_label_create(card_cam);
    lv_label_set_text(l_cam_sub, "Xem bên trong ngăn lạnh theo thời gian thực");
    lv_obj_set_style_text_color(l_cam_sub, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(l_cam_sub, &lv_font_vietnamese_11, 0);
    lv_obj_align(l_cam_sub, LV_ALIGN_TOP_LEFT, 54, 24);

    lv_obj_add_event_cb(card_cam, [](lv_event_t *e){
        ES8311_Audio::play_touch_sound(1600, 50);
    }, LV_EVENT_CLICKED, NULL);

    /* ----------------------------------------------------------------
     * TAB 3: TRỢ LÝ THÔNG MINH (ROBOT & AUDIO SPECTRUM)
     * ---------------------------------------------------------------- */
    create_screen_header(t3, "TRỢ LÝ AI", &lbl_time3);

    // 1. Compact Hero Card (Avatar Robot + Status & Hint)
    lv_obj_t *card_ai_hero = lv_obj_create(t3);
    lv_obj_set_size(card_ai_hero, 296, 68);
    lv_obj_align(card_ai_hero, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_bg_color(card_ai_hero, lv_color_hex(0x08101E), 0);
    lv_obj_set_style_border_color(card_ai_hero, lv_color_hex(0x15263F), 0);
    lv_obj_set_style_border_width(card_ai_hero, 1, 0);
    lv_obj_set_style_radius(card_ai_hero, 12, 0);
    lv_obj_set_style_pad_all(card_ai_hero, 6, 0);
    lv_obj_clear_flag(card_ai_hero, LV_OBJ_FLAG_SCROLLABLE);

    btn_robot_orb = lv_btn_create(card_ai_hero);
    lv_obj_set_size(btn_robot_orb, 52, 52);
    lv_obj_align(btn_robot_orb, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_radius(btn_robot_orb, 14, 0);
    lv_obj_set_style_bg_color(btn_robot_orb, lv_color_hex(0x0A1C36), 0);
    lv_obj_set_style_border_color(btn_robot_orb, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(btn_robot_orb, 2, 0);
    lv_obj_set_style_pad_all(btn_robot_orb, 0, 0);
    lv_obj_set_style_shadow_color(btn_robot_orb, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_shadow_width(btn_robot_orb, 8, 0);
    lv_obj_add_event_cb(btn_robot_orb, btn_agent_talk_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *img_r = lv_img_create(btn_robot_orb);
    lv_img_set_src(img_r, &img_robot_ai);
    lv_img_set_zoom(img_r, 158); // Zoom 84x84 down to 52x52
    lv_obj_center(img_r);

    lv_obj_t *l_hero_title = lv_label_create(card_ai_hero);
    lv_label_set_text(l_hero_title, "TRỢ LÝ MINIMAZING");
    lv_obj_set_style_text_color(l_hero_title, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(l_hero_title, &lv_font_vietnamese_11, 0);
    lv_obj_align(l_hero_title, LV_ALIGN_TOP_LEFT, 64, 4);

    lbl_ai_status = lv_label_create(card_ai_hero);
    lv_label_set_text(lbl_ai_status, LV_SYMBOL_OK " TRỰC TUYẾN");
    lv_obj_set_style_text_color(lbl_ai_status, lv_color_hex(0x10B981), 0);
    lv_obj_set_style_text_font(lbl_ai_status, &lv_font_vietnamese_11, 0);
    lv_obj_align(lbl_ai_status, LV_ALIGN_TOP_LEFT, 64, 20);

    lv_obj_t *l_agent_hint = lv_label_create(card_ai_hero);
    lv_label_set_text(l_agent_hint, "Chạm vào Robot để nói chuyện");
    lv_obj_set_style_text_color(l_agent_hint, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(l_agent_hint, &lv_font_vietnamese_11, 0);
    lv_obj_align(l_agent_hint, LV_ALIGN_TOP_LEFT, 64, 36);

    // 2. Large Dialogue Bubble (Expanded to 175px with vertical scrolling!)
    box_bubble = lv_obj_create(t3);
    lv_obj_set_size(box_bubble, 296, 175);
    lv_obj_align(box_bubble, LV_ALIGN_TOP_MID, 0, 106);
    lv_obj_set_style_bg_color(box_bubble, lv_color_hex(0x08101E), 0);
    lv_obj_set_style_border_color(box_bubble, lv_color_hex(0x15263F), 0);
    lv_obj_set_style_border_width(box_bubble, 1, 0);
    lv_obj_set_style_radius(box_bubble, 12, 0);
    lv_obj_set_style_pad_all(box_bubble, 8, 0);
    lv_obj_add_flag(box_bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(box_bubble, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box_bubble, LV_SCROLLBAR_MODE_AUTO);

    lbl_ai_bubble = lv_label_create(box_bubble);
    lv_label_set_text(lbl_ai_bubble, "Xin chào bạn! Hãy chạm vào Robot để đặt câu hỏi bất kỳ (đời sống, khoa học, nấu ăn, mẹo vặt, tình trạng tủ lạnh...).");
    lv_label_set_long_mode(lbl_ai_bubble, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_ai_bubble, 276);
    lv_obj_set_style_text_color(lbl_ai_bubble, lv_color_hex(0xF8FAFC), 0);
    lv_obj_set_style_text_font(lbl_ai_bubble, &lv_font_vietnamese_11, 0);
    lv_obj_align(lbl_ai_bubble, LV_ALIGN_TOP_LEFT, 0, 0);

    // 3. Compact Spectrum Visualizer (Slim 42px placed below dialogue bubble)
    lv_obj_t *card_spec = lv_obj_create(t3);
    lv_obj_set_size(card_spec, 296, 42);
    lv_obj_align(card_spec, LV_ALIGN_TOP_MID, 0, 287);
    lv_obj_set_style_bg_color(card_spec, lv_color_hex(0x08101E), 0);
    lv_obj_set_style_border_color(card_spec, lv_color_hex(0x15263F), 0);
    lv_obj_set_style_border_width(card_spec, 1, 0);
    lv_obj_set_style_radius(card_spec, 10, 0);
    lv_obj_set_style_pad_all(card_spec, 4, 0);
    lv_obj_clear_flag(card_spec, LV_OBJ_FLAG_SCROLLABLE);

    lbl_mic_db = lv_label_create(card_spec);
    lv_label_set_text(lbl_mic_db, "Mic: 0%");
    lv_obj_set_style_text_color(lbl_mic_db, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(lbl_mic_db, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_mic_db, LV_ALIGN_LEFT_MID, 6, -3);

    lv_obj_t *viz_cont = lv_obj_create(card_spec);
    lv_obj_set_size(viz_cont, 216, 22);
    lv_obj_align(viz_cont, LV_ALIGN_RIGHT_MID, -4, -3);
    lv_obj_set_style_bg_opa(viz_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(viz_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(viz_cont, 0, 0);
    lv_obj_clear_flag(viz_cont, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < SPECTRUM_BARS; i++) {
        bar_viz[i] = lv_obj_create(viz_cont);
        lv_obj_set_size(bar_viz[i], 10, 4);
        lv_obj_align(bar_viz[i], LV_ALIGN_BOTTOM_LEFT, i * 14 + 4, 0);
        lv_obj_set_style_bg_color(bar_viz[i], lv_color_hex(0x0284C7), 0);
        lv_obj_set_style_border_width(bar_viz[i], 0, 0);
        lv_obj_set_style_radius(bar_viz[i], 2, 0);
    }

    bar_vu = lv_bar_create(card_spec);
    lv_bar_set_range(bar_vu, 0, 100);
    lv_bar_set_value(bar_vu, 0, LV_ANIM_OFF);
    lv_obj_set_size(bar_vu, 284, 3);
    lv_obj_align(bar_vu, LV_ALIGN_BOTTOM_MID, 0, -1);
    lv_obj_set_style_bg_color(bar_vu, lv_color_hex(0x132238), 0);
    lv_obj_set_style_bg_color(bar_vu, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_vu, 2, 0);

    // 4. Voice Test Button (Placed neatly above navbar)
    lv_obj_t *btn_test_rec = lv_btn_create(t3);
    lv_obj_set_size(btn_test_rec, 296, 38);
    lv_obj_align(btn_test_rec, LV_ALIGN_TOP_MID, 0, 335);
    lv_obj_set_style_radius(btn_test_rec, 10, 0);
    lv_obj_set_style_bg_color(btn_test_rec, lv_color_hex(0x0F766E), 0);
    lv_obj_set_style_border_color(btn_test_rec, lv_color_hex(0x2DD4BF), 0);
    lv_obj_set_style_border_width(btn_test_rec, 1, 0);
    lv_obj_add_event_cb(btn_test_rec, btn_test_record_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l_rec = lv_label_create(btn_test_rec);
    lv_label_set_text(l_rec, LV_SYMBOL_AUDIO "  THỬ & PHÁT LẠI GIỌNG NÓI (2s)");
    lv_obj_set_style_text_color(l_rec, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l_rec, &lv_font_vietnamese_11, 0);
    lv_obj_center(l_rec);

    /* ----------------------------------------------------------------
     * TAB 4: CÀI ĐẶT (EXACT FIGMA REPLICA)
     * ---------------------------------------------------------------- */
    create_screen_header(t4, "CÀI ĐẶT", &lbl_time4);

    lv_obj_t *set_cont = lv_obj_create(t4);
    lv_obj_set_size(set_cont, 296, 376);
    lv_obj_align(set_cont, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_opa(set_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(set_cont, 0, 0);
    lv_obj_set_style_pad_all(set_cont, 0, 0);
    lv_obj_set_flex_flow(set_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(set_cont, 6, 0);
    lv_obj_set_scroll_dir(set_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(set_cont, LV_SCROLLBAR_MODE_AUTO);

    // Helper lambda for creating settings item cards
    auto create_setting_card = [](lv_obj_t *parent, int height = 40) -> lv_obj_t* {
        lv_obj_t *card = lv_obj_create(parent);
        lv_obj_set_size(card, 296, height);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x08101E), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x15263F), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        return card;
    };

    auto create_icon_box = [](lv_obj_t *card, const lv_img_dsc_t *icon_dsc, uint32_t bg_hex = 0x0E1C33) -> lv_obj_t* {
        lv_obj_t *box = lv_obj_create(card);
        lv_obj_set_size(box, 28, 28);
        lv_obj_align(box, LV_ALIGN_LEFT_MID, 2, 0);
        lv_obj_set_style_bg_color(box, lv_color_hex(bg_hex), 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_radius(box, 6, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *ic = lv_img_create(box);
        lv_img_set_src(ic, icon_dsc);
        lv_obj_center(ic);
        return box;
    };

    // 1. Wi-Fi Card
    lv_obj_t *c_wifi = create_setting_card(set_cont, 44);
    create_icon_box(c_wifi, &icon_droplet_blue, 0x0B2A4A);
    lv_obj_t *l_w_t = lv_label_create(c_wifi);
    lv_label_set_text(l_w_t, "Kết nối Wi-Fi");
    lv_obj_set_style_text_color(l_w_t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l_w_t, &lv_font_vietnamese_14, 0);
    lv_obj_align(l_w_t, LV_ALIGN_LEFT_MID, 40, -8);

    lbl_rw_wifi = lv_label_create(c_wifi);
    lv_label_set_text(lbl_rw_wifi, "MiniMazing_5G");
    lv_obj_set_style_text_color(lbl_rw_wifi, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(lbl_rw_wifi, &lv_font_vietnamese_11, 0);
    lv_obj_align(lbl_rw_wifi, LV_ALIGN_LEFT_MID, 40, 8);

    lv_obj_t *l_w_arr = lv_label_create(c_wifi);
    lv_label_set_text(l_w_arr, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(l_w_arr, lv_color_hex(0x64748B), 0);
    lv_obj_align(l_w_arr, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_add_event_cb(c_wifi, [](lv_event_t *e){ open_wifi_manager_screen(); }, LV_EVENT_CLICKED, NULL);

    // 2. Battery & Power Card (Thời lượng pin & Tắt kêu pin yếu)
    lv_obj_t *c_bat = create_setting_card(set_cont, 44);
    create_icon_box(c_bat, &icon_shield_green, 0x0E2A1E);
    lv_obj_t *l_bat_title = lv_label_create(c_bat);
    lv_label_set_text(l_bat_title, "Thời lượng pin");
    lv_obj_set_style_text_color(l_bat_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l_bat_title, &lv_font_vietnamese_14, 0);
    lv_obj_align(l_bat_title, LV_ALIGN_LEFT_MID, 40, -8);

    lbl_bat_sub = lv_label_create(c_bat);
    lv_label_set_text(lbl_bat_sub, "Bảo vệ pin: Im lặng");
    lv_obj_set_style_text_color(lbl_bat_sub, lv_color_hex(0x64748B), 0);
    lv_obj_set_style_text_font(lbl_bat_sub, &lv_font_vietnamese_11, 0);
    lv_obj_align(lbl_bat_sub, LV_ALIGN_LEFT_MID, 40, 8);

    lbl_bat_val = lv_label_create(c_bat);
    lv_label_set_text_fmt(lbl_bat_val, "%d%% (%d.%02dV)", s_battery_pct, s_battery_mv / 1000, (s_battery_mv % 1000) / 10);
    lv_obj_set_style_text_color(lbl_bat_val, lv_color_hex(0x22C55E), 0);
    lv_obj_set_style_text_font(lbl_bat_val, &lv_font_vietnamese_11, 0);
    lv_obj_align(lbl_bat_val, LV_ALIGN_RIGHT_MID, -6, 0);

    // 3. Notifications Card
    lv_obj_t *c_notif = create_setting_card(set_cont, 40);
    create_icon_box(c_notif, &icon_bell_yellow, 0x1E2238);
    lv_obj_t *l_notif = lv_label_create(c_notif);
    lv_label_set_text(l_notif, "Thông báo");
    lv_obj_set_style_text_color(l_notif, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l_notif, &lv_font_vietnamese_14, 0);
    lv_obj_align(l_notif, LV_ALIGN_LEFT_MID, 40, 0);

    sw_settings_noti = lv_switch_create(c_notif);
    lv_obj_set_size(sw_settings_noti, 38, 20);
    lv_obj_align(sw_settings_noti, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_color(sw_settings_noti, lv_color_hex(0x00E5FF), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_state(sw_settings_noti, LV_STATE_CHECKED);

    // 3. Child Lock Card
    lv_obj_t *c_lock = create_setting_card(set_cont, 40);
    create_icon_box(c_lock, &icon_lock_amber, 0x2A2016);
    lv_obj_t *l_lock = lv_label_create(c_lock);
    lv_label_set_text(l_lock, "Khóa trẻ em");
    lv_obj_set_style_text_color(l_lock, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l_lock, &lv_font_vietnamese_14, 0);
    lv_obj_align(l_lock, LV_ALIGN_LEFT_MID, 40, 0);

    sw_settings_lock = lv_switch_create(c_lock);
    lv_obj_set_size(sw_settings_lock, 38, 20);
    lv_obj_align(sw_settings_lock, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_color(sw_settings_lock, lv_color_hex(0x00E5FF), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_settings_lock, [](lv_event_t *e){
        s_child_lock = lv_obj_has_state(sw_settings_lock, LV_STATE_CHECKED);
        if (lbl_home_lock_state) {
            lv_label_set_text(lbl_home_lock_state, s_child_lock ? "Đang bật" : "Tắt");
            lv_obj_set_style_text_color(lbl_home_lock_state, lv_color_hex(s_child_lock ? 0xF59E0B : 0x64748B), 0);
        }
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // 4. Eco Mode Card
    lv_obj_t *c_eco = create_setting_card(set_cont, 40);
    create_icon_box(c_eco, &icon_leaf_green, 0x0E2A1E);
    lv_obj_t *l_eco = lv_label_create(c_eco);
    lv_label_set_text(l_eco, "Tiết kiệm điện");
    lv_obj_set_style_text_color(l_eco, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l_eco, &lv_font_vietnamese_14, 0);
    lv_obj_align(l_eco, LV_ALIGN_LEFT_MID, 40, 0);

    sw_settings_eco = lv_switch_create(c_eco);
    lv_obj_set_size(sw_settings_eco, 38, 20);
    lv_obj_align(sw_settings_eco, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_color(sw_settings_eco, lv_color_hex(0x00E5FF), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_state(sw_settings_eco, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_settings_eco, [](lv_event_t *e){
        s_eco_mode = lv_obj_has_state(sw_settings_eco, LV_STATE_CHECKED);
        if (lbl_home_eco_state) {
            lv_label_set_text(lbl_home_eco_state, s_eco_mode ? "Đang bật" : "Tắt");
            lv_obj_set_style_text_color(lbl_home_eco_state, lv_color_hex(s_eco_mode ? 0x22C55E : 0x64748B), 0);
        }
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // 5. Brightness Card
    lv_obj_t *c_br = create_setting_card(set_cont, 40);
    create_icon_box(c_br, &icon_sun_yellow, 0x2A2410);
    lv_obj_t *l_br = lv_label_create(c_br);
    lv_label_set_text(l_br, "Độ sáng");
    lv_obj_set_style_text_color(l_br, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l_br, &lv_font_vietnamese_14, 0);
    lv_obj_align(l_br, LV_ALIGN_LEFT_MID, 40, 0);

    lv_obj_t *sb_b = lv_slider_create(c_br);
    lv_slider_set_range(sb_b, 10, 100);
    lv_slider_set_value(sb_b, screen_brightness, LV_ANIM_OFF);
    lv_obj_set_size(sb_b, 110, 8);
    lv_obj_align(sb_b, LV_ALIGN_RIGHT_MID, -44, 0);
    lv_obj_set_style_bg_color(sb_b, lv_color_hex(0x1E2E4E), 0);
    lv_obj_set_style_bg_color(sb_b, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sb_b, lv_color_hex(0x00E5FF), LV_PART_KNOB);
    lv_obj_add_event_cb(sb_b, slider_bright_event, LV_EVENT_VALUE_CHANGED, NULL);

    lbl_bright_pct = lv_label_create(c_br);
    lv_label_set_text_fmt(lbl_bright_pct, "%d%%", screen_brightness);
    lv_obj_set_style_text_color(lbl_bright_pct, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(lbl_bright_pct, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_bright_pct, LV_ALIGN_RIGHT_MID, -6, 0);

    // 6. Sound Volume Card
    lv_obj_t *c_vol = create_setting_card(set_cont, 40);
    create_icon_box(c_vol, &icon_speaker_purple, 0x221235);
    lv_obj_t *l_vol = lv_label_create(c_vol);
    lv_label_set_text(l_vol, "Âm thanh");
    lv_obj_set_style_text_color(l_vol, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l_vol, &lv_font_vietnamese_14, 0);
    lv_obj_align(l_vol, LV_ALIGN_LEFT_MID, 40, 0);

    lv_obj_t *sb_v = lv_slider_create(c_vol);
    lv_slider_set_range(sb_v, 0, 100);
    lv_slider_set_value(sb_v, sound_volume, LV_ANIM_OFF);
    lv_obj_set_size(sb_v, 110, 8);
    lv_obj_align(sb_v, LV_ALIGN_RIGHT_MID, -44, 0);
    lv_obj_set_style_bg_color(sb_v, lv_color_hex(0x1E2E4E), 0);
    lv_obj_set_style_bg_color(sb_v, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sb_v, lv_color_hex(0x00E5FF), LV_PART_KNOB);
    lv_obj_add_event_cb(sb_v, slider_vol_event, LV_EVENT_VALUE_CHANGED, NULL);

    lbl_vol_pct = lv_label_create(c_vol);
    lv_label_set_text_fmt(lbl_vol_pct, "%d%%", sound_volume);
    lv_obj_set_style_text_color(lbl_vol_pct, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(lbl_vol_pct, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_vol_pct, LV_ALIGN_RIGHT_MID, -6, 0);

    // 7. Screen Timeout Card
    lv_obj_t *c_timeout = create_setting_card(set_cont, 40);
    create_icon_box(c_timeout, &icon_bell_yellow, 0x1E2238);
    lv_obj_t *l_to = lv_label_create(c_timeout);
    lv_label_set_text(l_to, "Chờ màn hình");
    lv_obj_set_style_text_color(l_to, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l_to, &lv_font_vietnamese_14, 0);
    lv_obj_align(l_to, LV_ALIGN_LEFT_MID, 40, 0);

    lbl_timeout_val = lv_label_create(c_timeout);
    lv_label_set_text(lbl_timeout_val, s_timeout_labels[s_timeout_idx]);
    lv_obj_set_style_text_color(lbl_timeout_val, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(lbl_timeout_val, &lv_font_vietnamese_11, 0);
    lv_obj_align(lbl_timeout_val, LV_ALIGN_RIGHT_MID, -24, 0);

    lv_obj_t *l_to_arr = lv_label_create(c_timeout);
    lv_label_set_text(l_to_arr, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(l_to_arr, lv_color_hex(0x64748B), 0);
    lv_obj_align(l_to_arr, LV_ALIGN_RIGHT_MID, -6, 0);

    lv_obj_add_event_cb(c_timeout, [](lv_event_t *e){
        s_timeout_idx = (s_timeout_idx + 1) % 5;
        s_screen_timeout_sec = s_timeout_options[s_timeout_idx];
        if (lbl_timeout_val) {
            lv_label_set_text(lbl_timeout_val, s_timeout_labels[s_timeout_idx]);
        }
        s_last_activity_time = millis();
        ES8311_Audio::play_touch_sound(1200 + s_timeout_idx * 80, 25);
    }, LV_EVENT_CLICKED, NULL);

    // 8. Device Info Row
    lv_obj_t *c_info = create_setting_card(set_cont, 40);
    create_icon_box(c_info, &icon_info_cyan, 0x0C253B);
    lv_obj_t *l_di = lv_label_create(c_info);
    lv_label_set_text(l_di, "Thông tin thiết bị");
    lv_obj_set_style_text_color(l_di, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l_di, &lv_font_vietnamese_14, 0);
    lv_obj_align(l_di, LV_ALIGN_LEFT_MID, 40, 0);
    lv_obj_t *l_di_arr = lv_label_create(c_info);
    lv_label_set_text(l_di_arr, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(l_di_arr, lv_color_hex(0x64748B), 0);
    lv_obj_align(l_di_arr, LV_ALIGN_RIGHT_MID, -6, 0);

    // 8. Language Row
    lv_obj_t *c_lang = create_setting_card(set_cont, 40);
    create_icon_box(c_lang, &icon_lang_purple, 0x221235);
    lv_obj_t *l_lg = lv_label_create(c_lang);
    lv_label_set_text(l_lg, "Ngôn ngữ");
    lv_obj_set_style_text_color(l_lg, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l_lg, &lv_font_vietnamese_14, 0);
    lv_obj_align(l_lg, LV_ALIGN_LEFT_MID, 40, 0);

    lv_obj_t *l_lg_val = lv_label_create(c_lang);
    lv_label_set_text(l_lg_val, "Tiếng Việt " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(l_lg_val, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(l_lg_val, &lv_font_vietnamese_11, 0);
    lv_obj_align(l_lg_val, LV_ALIGN_RIGHT_MID, -6, 0);

    // 9. Bottom Neon Fridge Wireframe Device Card
    lv_obj_t *c_neon_card = lv_obj_create(set_cont);
    lv_obj_set_size(c_neon_card, 296, 76);
    lv_obj_set_style_bg_color(c_neon_card, lv_color_hex(0x08101E), 0);
    lv_obj_set_style_border_color(c_neon_card, lv_color_hex(0x15263F), 0);
    lv_obj_set_style_border_width(c_neon_card, 1, 0);
    lv_obj_set_style_radius(c_neon_card, 12, 0);
    lv_obj_set_style_pad_all(c_neon_card, 4, 0);
    lv_obj_clear_flag(c_neon_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img_neon = lv_img_create(c_neon_card);
    lv_img_set_src(img_neon, &img_fridge_neon);
    lv_obj_align(img_neon, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *l_nd_t = lv_label_create(c_neon_card);
    lv_label_set_text(l_nd_t, "TỦ LẠNH THÔNG MINH");
    lv_obj_set_style_text_color(l_nd_t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l_nd_t, &lv_font_vietnamese_11, 0);
    lv_obj_align(l_nd_t, LV_ALIGN_TOP_LEFT, 72, 8);

    lv_obj_t *l_nd_v = lv_label_create(c_neon_card);
    lv_label_set_text(l_nd_v, "Phiên bản: 1.2.3");
    lv_obj_set_style_text_color(l_nd_v, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(l_nd_v, &lv_font_vietnamese_11, 0);
    lv_obj_align(l_nd_v, LV_ALIGN_TOP_LEFT, 72, 26);

    lv_obj_t *l_nd_id = lv_label_create(c_neon_card);
    lv_label_set_text(l_nd_id, "Mã thiết bị: TL-S3-35D-2026");
    lv_obj_set_style_text_color(l_nd_id, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(l_nd_id, &lv_font_vietnamese_11, 0);
    lv_obj_align(l_nd_id, LV_ALIGN_TOP_LEFT, 72, 44);

    /* ----------------------------------------------------------------
     * FOOTER BOTTOM NAVIGATION (BITMAP ICONS & CLEAN VIETNAMESE TYPOGRAPHY)
     * ---------------------------------------------------------------- */
    const lv_img_dsc_t *f_act_icons[] = {&icon_nav_home_act, &icon_nav_fridge_act, &icon_nav_robot_act, &icon_nav_gear_act};
    const lv_img_dsc_t *f_inact_icons[] = {&icon_nav_home_inact, &icon_nav_fridge_inact, &icon_nav_robot_inact, &icon_nav_gear_inact};
    const char *f_texts[] = {"Trang chủ", "Ngăn lạnh", "Trợ lý AI", "Cài đặt"};

    for (int i = 0; i < 4; i++) {
        footer_btn[i] = lv_btn_create(lv_scr_act());
        lv_obj_set_size(footer_btn[i], 80, 52);
        lv_obj_align(footer_btn[i], LV_ALIGN_BOTTOM_LEFT, i * 80, 0);
        lv_obj_set_style_radius(footer_btn[i], 0, 0);
        lv_obj_set_style_bg_color(footer_btn[i], (i == 0) ? lv_color_hex(0x0C1628) : lv_color_hex(0x060B18), 0);
        lv_obj_set_style_pad_all(footer_btn[i], 2, 0);

        if (i == 0) {
            lv_obj_set_style_border_side(footer_btn[i], LV_BORDER_SIDE_TOP, 0);
            lv_obj_set_style_border_color(footer_btn[i], lv_color_hex(0x00E5FF), 0);
            lv_obj_set_style_border_width(footer_btn[i], 2, 0);
        } else {
            lv_obj_set_style_border_width(footer_btn[i], 0, 0);
        }
        lv_obj_add_event_cb(footer_btn[i], footer_nav_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        footer_img[i] = lv_img_create(footer_btn[i]);
        lv_img_set_src(footer_img[i], (i == 0) ? f_act_icons[i] : f_inact_icons[i]);
        lv_obj_align(footer_img[i], LV_ALIGN_TOP_MID, 0, 4);

        footer_lbl[i] = lv_label_create(footer_btn[i]);
        lv_label_set_text(footer_lbl[i], f_texts[i]);
        lv_obj_set_style_text_align(footer_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(footer_lbl[i], &lv_font_vietnamese_11, 0);
        lv_obj_set_style_text_color(footer_lbl[i], (i == 0) ? lv_color_hex(0x00E5FF) : lv_color_hex(0x64748B), 0);
        lv_obj_align(footer_lbl[i], LV_ALIGN_BOTTOM_MID, 0, -2);
    }
}

/* ====================================================================
 * ARDUINO SETUP & LOOP
 * ==================================================================== */
void setup() {
    Serial.begin(115200);
    delay(400);
    LicenseGuard::init();
    if (psramInit()) {
        Serial.printf("[PSRAM] Initialized! Total: %d bytes, Free: %d bytes\n", ESP.getPsramSize(), ESP.getFreePsram());
    } else {
        Serial.println("[PSRAM] psramInit() failed or no PSRAM");
    }
    Serial.println("\n[INIT] Starting MiniMazing Smart Fridge UI (Clean Typography, Zero Tofu)...");

    // 1. Backlight on GPIO 41
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);
    ledcSetup(0, 5000, 8);
    ledcAttachPin(LCD_BL, 0);
    set_backlight(75);

    // 2. Hardware Reset for Touch (GPIO 48 RST, PR #2112)
    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, LOW);
    delay(10);
    digitalWrite(TOUCH_RST, HIGH);
    delay(100);

    // 3. Initialize ESP-IDF I2C Driver on GPIO 38 (SDA), 39 (SCL)
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = (gpio_num_t)TOUCH_SDA;
    conf.scl_io_num = (gpio_num_t)TOUCH_SCL;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 400000;
    i2c_param_config(I2C_NUM_0, &conf);
    i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0);

    uint8_t max_pts = 1;
    if (touch_i2c_read(0x0009, &max_pts, 1) && max_pts > 0 && max_pts <= 5) {
        touch_max_points = max_pts;
        Serial.printf("[INIT] Touch ST77922 ready (max touches: %d)\n", touch_max_points);
    }

    // 4. Initialize Audio Codec ES8311 (Mode 3: DOUT=15, DIN=16, PA=LOW (0) Active SC8002B)
    ES8311_Audio::init(SPEAKER_PA_EN, I2S_MCLK, I2S_BCLK, I2S_WS, I2S_DOUT, I2S_DIN, 3);
    ES8311_Audio::set_volume(100);       // Max volume (100%)
    // Khởi động hoàn toàn yên lặng (không phát chuông báo khi bật nguồn)

    // 5. Initialize ST77922 Display
    tft.begin(40000000);
    tft.init();
    tft.writeCommand(0x21); // Turn ON Inversion (0x21) -> True colors (Dark theme, not white)

    // 6. Initialize LVGL
    lv_init(); // CRITICAL: Must be called before any LVGL memory buffers or drivers!
    disp_buf1 = (lv_color_t *)heap_caps_malloc(SCREEN_WIDTH * BUF_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA);
    disp_buf2 = (lv_color_t *)heap_caps_malloc(SCREEN_WIDTH * BUF_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (!disp_buf1) disp_buf1 = (lv_color_t *)malloc(SCREEN_WIDTH * BUF_LINES * sizeof(lv_color_t));
    if (!disp_buf2) disp_buf2 = (lv_color_t *)malloc(SCREEN_WIDTH * BUF_LINES * sizeof(lv_color_t));
    lv_disp_draw_buf_init(&draw_buf, disp_buf1, disp_buf2, SCREEN_WIDTH * BUF_LINES);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.rounder_cb = my_rounder_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    // 7. Build UI with zero tofu boxes
    build_smart_fridge_ui();
    Serial.println("[INIT] Clean Typography UI with zero tofu loaded!");

    // Bản quyền vĩnh viễn (Đã gỡ bỏ giới hạn 1000 lần khởi động)
    Serial.println("[SECURITY] Lifetime License Active. Device fully unlocked.");

    // 8. Create Live Microphone Visualizer Timer (80ms interval)
    mic_viz_timer = lv_timer_create(mic_visualizer_timer_cb, 80, NULL);

    // 9. Auto-connect to saved WiFi credentials from Preferences NVS (or user configured in config_ai.h)
    wifi_prefs.begin("wifi_cfg", false);
    cur_wifi_ssid = wifi_prefs.getString("ssid", "");
    cur_wifi_pass = wifi_prefs.getString("pass", "");
    if (strlen(USER_WIFI_SSID) > 0) {
        cur_wifi_ssid = USER_WIFI_SSID;
        cur_wifi_pass = USER_WIFI_PASS;
        wifi_prefs.putString("ssid", cur_wifi_ssid);
        wifi_prefs.putString("pass", cur_wifi_pass);
        Serial.printf("[WIFI] Set USER configured network: %s\n", cur_wifi_ssid.c_str());
    } else if (cur_wifi_ssid.length() == 0) {
        cur_wifi_ssid = "VIETSET_TECH";
        cur_wifi_pass = "vs68686868";
        wifi_prefs.putString("ssid", cur_wifi_ssid);
        wifi_prefs.putString("pass", cur_wifi_pass);
        Serial.println("[WIFI] Set default network: VIETSET_TECH / vs68686868");
    }
    wifi_prefs.end();

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);          // Passive non-blocking background reconnection
    WiFi.setSleep(false);                 // Disable sleep: maximum Wi-Fi sensitivity
    WiFi.setTxPower(WIFI_POWER_19_5dBm);  // Maximum RF power: 20dBm

    wifi_country_t country = {
        .cc = "VN",
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_AUTO
    };
    esp_wifi_set_country(&country);

    Serial.printf("[WIFI] Auto-reconnecting to network: %s\n", cur_wifi_ssid.c_str());
    if (cur_wifi_pass.length() > 0) {
        WiFi.begin(cur_wifi_ssid.c_str(), cur_wifi_pass.c_str());
    } else {
        WiFi.begin(cur_wifi_ssid.c_str());
    }
    update_wifi_ui_status();

    s_last_activity_time = millis();

    // Khởi tạo đo pin & thiết lập bảo vệ im lặng
    pinMode(BAT_ADC_PIN, INPUT);
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
    read_battery_voltage();
    lv_timer_create([](lv_timer_t *t){
        read_battery_voltage();
    }, 4000, NULL);
}

void loop() {
    lv_timer_handler();

    // Wi-Fi Auto-Reconnect: Duy trì kết nối ngầm mượt mà, không block CPU hay làm giật màn hình
    static uint32_t s_last_wifi_retry = 0;
    if (WiFi.status() != WL_CONNECTED && !wifi_full_screen && !s_wifi_scanning_active) {
        if (cur_wifi_ssid.length() > 0 && (millis() - s_last_wifi_retry > 45000)) {
            s_last_wifi_retry = millis();
            Serial.printf("[WIFI] Auto-reconnecting to: %s...\n", cur_wifi_ssid.c_str());
            WiFi.reconnect();
            update_wifi_ui_status();
        }
    }

    // Screen Auto-Timeout Check (Dim / Turn off backlight when idle)
    if (s_screen_timeout_sec > 0 && !s_screen_sleeping) {
        if (millis() - s_last_activity_time >= (uint32_t)s_screen_timeout_sec * 1000) {
            s_screen_sleeping = true;
            set_backlight(0);
        }
    }

    // Real-time footer navigation highlight synchronization (swipes, gestures & clicks)
    static uint16_t s_last_active_tab = 0;
    if (tv) {
        uint16_t cur_t = lv_tabview_get_tab_act(tv);
        if (cur_t != s_last_active_tab) {
            s_last_active_tab = cur_t;
            update_nav_highlight(cur_t);
        }
    }

    static bool wifi_connected_prev = false;
    bool is_now_connected = (WiFi.status() == WL_CONNECTED);
    if (is_now_connected && !wifi_connected_prev) {
        wifi_connected_prev = true;
        Serial.printf("[WIFI] Connected successfully! IP: %s\n", WiFi.localIP().toString().c_str());
        // License check removed for zero-lag
        if (lbl_ai_status) {
            lv_label_set_text(lbl_ai_status, LV_SYMBOL_WIFI " TRỰC TUYẾN (WIFI)");
            lv_obj_set_style_text_color(lbl_ai_status, lv_color_hex(0x00E5FF), 0);
        }
        configTime(0, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");
        update_wifi_ui_status();
    } else if (!is_now_connected && wifi_connected_prev) {
        wifi_connected_prev = false;
        if (lbl_ai_status) {
            lv_label_set_text(lbl_ai_status, LV_SYMBOL_OK " SẴN SÀNG");
            lv_obj_set_style_text_color(lbl_ai_status, lv_color_hex(0x10B981), 0);
        }
        update_wifi_ui_status();
    }

    // Auto-update real-time clock every 1 second
    static uint32_t last_time_update = 0;
    if (millis() - last_time_update >= 1000) {
        last_time_update = millis();
        update_time_ui();
    }

    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.startsWith("KEY:")) {
            String key = line.substring(4);
            key.trim();
            if (LicenseGuard::verify_and_activate(key.c_str())) {
                Serial.println("[LICENSE] >>> KÍCH HOẠT BẢN QUYỀN THÀNH CÔNG QUA CỔNG SERIAL! <<<");
                ES8311_Audio::play_startup_chime();
                if (s_license_modal) {
                    lv_obj_del(s_license_modal);
                    s_license_modal = nullptr;
                }
                if (lbl_setting_license_sub) {
                    lv_label_set_text(lbl_setting_license_sub, LicenseGuard::get_license_status_string().c_str());
                    lv_obj_set_style_text_color(lbl_setting_license_sub, lv_color_hex(0x10B981), 0);
                }
            } else {
                Serial.println("[LICENSE] !!! MÃ KEY BẢN QUYỀN KHÔNG ĐÚNG !!!");
            }
        } else if (line.length() > 0) {
            char c = line.charAt(0);
            if (c == '0') { ES8311_Audio::set_hardware_mode(0); ES8311_Audio::play_test_melody(); }
            else if (c == '1') { ES8311_Audio::set_hardware_mode(1); ES8311_Audio::play_test_melody(); }
            else if (c == '2') { ES8311_Audio::set_hardware_mode(2); ES8311_Audio::play_test_melody(); }
            else if (c == '3') { ES8311_Audio::set_hardware_mode(3); ES8311_Audio::play_test_melody(); }
            else if (c == 't') { ES8311_Audio::play_test_melody(); }
            else if (c == 's') { ES8311_Audio::test_all_modes_chime(); }
            else if (c == 'm') { ES8311_Audio::dump_mic_raw(); }
            else if (c == 'i') {
                static bool s_inv = true;
                s_inv = !s_inv;
                tft.writeCommand(s_inv ? 0x21 : 0x20);
                Serial.printf("[DISPLAY] Inversion toggled to: 0x%02X (%s)\n", s_inv ? 0x21 : 0x20, s_inv ? "0x21 ON (Dark mode)" : "0x20 OFF (White mode)");
            }
            else if (c == 'v') {
                Serial.println("[TEST] Testing Google Translate TTS Voice...");
                GoogleTTS::speak("Xin chào bạn! Trợ lý ảo MiniMazing đã sẵn sàng hỗ trợ bạn!", on_tts_speech_level);
            }
        }
    }

    delay(1);
}
