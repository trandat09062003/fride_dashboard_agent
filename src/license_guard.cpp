#include "license_guard.h"

namespace LicenseGuard {

void init() {
    Serial.println("[LICENSE] Lifetime License Active. No 1000-boot limit or remote lock.");
}

bool is_activated() {
    return true;
}

bool is_locked() {
    return false;
}

uint32_t get_boot_count() {
    return 1;
}

uint32_t get_trial_limit() {
    return 999999;
}

bool verify_and_activate(const char *input_key) {
    return true;
}

void check_github_license() {
    // Disabled: Eliminates network lag and HTTPS freezing
}

String get_license_status_string() {
    return "Đã kích hoạt bản quyền Vĩnh Viễn";
}

} // namespace LicenseGuard
