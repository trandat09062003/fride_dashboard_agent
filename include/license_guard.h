#pragma once

#include <Arduino.h>

namespace LicenseGuard {

    // Khởi tạo hệ thống bảo mật & tăng số lần khởi động (lưu vào NVS)
    void init();

    // Kiểm tra trạng thái kích hoạt vĩnh viễn
    bool is_activated();

    // Kiểm tra xem thiết bị có đang bị khóa (hết 1000 lần mà chưa có key) không
    bool is_locked();

    // Lấy số lần khởi động hiện tại
    uint32_t get_boot_count();

    // Lấy giới hạn số lần dùng thử (mặc định 1000)
    uint32_t get_trial_limit();

    // Xác thực key nhập vào (Băm Salted SHA-256 đối chiếu bảo mật)
    // Nếu đúng, lưu vĩnh viễn vào NVS và mở khóa ngay lập tức
    bool verify_and_activate(const char *input_key);

    // Kiểm tra đồng bộ bản quyền từ GitHub khi có kết nối WiFi
    void check_github_license();

    // Lấy chuỗi mô tả trạng thái bản quyền (để hiển thị trên UI)
    String get_license_status_string();

} // namespace LicenseGuard
