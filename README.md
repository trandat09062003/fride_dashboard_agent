# Smart Fridge Dashboard Agent (ESP32-S3 3.5" IPS) ❄️🤖

<p align="center">
  <img src="docs/images/design_header_badge.png" alt="Smart Fridge Banner" width="700"/>
</p>

Hệ thống bảng điều khiển trung tâm dành cho **Tủ Lạnh Thông Minh (Smart Refrigerator Dashboard)** vận hành trên vi điều khiển **ESP32-S3 WROOM-1** kết hợp màn hình cảm ứng điện dung 3.5 inch IPS (320x480) và bộ giải mã âm thanh chuyên dụng Everest ES8311.

---

## 1. Giao Diện & Tính Năng Nổi Bật

<p align="center">
  <img src="docs/images/design_page1_home.png" width="45%" alt="Trang Chủ"/>
  <img src="docs/images/design_page2_cooling.png" width="45%" alt="Điều Khiển Nhiệt Độ"/>
</p>
<p align="center">
  <img src="docs/images/design_page3_assistant.png" width="45%" alt="Trợ Lý AI Voice"/>
  <img src="docs/images/design_page4_settings.png" width="45%" alt="Cài Đặt Wi-Fi"/>
</p>

* 🧊 **Dark Mode UI Hiện Đại**: Xây dựng trên nền tảng đồ họa LVGL 8.3, tối ưu bộ đệm màn hình PSRAM 8MB cho hiệu năng 60 FPS mượt mà.
* ❄️ **Quản Lý Làm Lạnh Đa Vùng**: Điều khiển nhiệt độ ngăn mát (0°C ~ 8°C) và ngăn đông (-24°C ~ -14°C) bằng slider trực quan kèm các chế độ làm lạnh nhanh (Super Cool, Super Freeze, Eco Mode).
* 🎙️ **Trợ Lý Giọng Nói AI**: Tích hợp pipeline thu âm qua I2S Codec ES8311, hiệu ứng sóng âm động (waveform micro) và kết nối trí tuệ nhân tạo Groq / Llama-3.
* 📶 **Quản Lý Wi-Fi & Đồng Bộ Giờ NTP**: Bàn phím ảo trực tiếp trên màn hình, quét và kết nối mạng Wi-Fi tự động, đồng bộ thời gian chuẩn Việt Nam (GMT+7).
* 💡 **Tiết Kiệm Điện Năng Thông Minh**: Tự động giảm độ sáng và tắt đèn nền (Backlight Timeout) sau 30 giây không tương tác; chạm nhẹ vào màn hình để đánh thức lập tức.

---

## 2. Sơ Đồ Kiến Trúc Phần Cứng (Hardware Pinout)

| Ngoại Vi | Chân ESP32-S3 | Chức Năng Chi Tiết |
| :--- | :--- | :--- |
| **Màn hình LCD (QSPI/SPI)** | | **Controller ST7796S / ST77922 (320x480 IPS)** |
| | `GPIO 10` | Chip Select (CS) |
| | `GPIO 12` | Serial Clock (SCK) |
| | `GPIO 11` | Data Line 0 (D0) |
| | `GPIO 13` | Data Line 1 (D1) |
| | `GPIO 14` | Data Line 2 (D2) |
| | `GPIO 9`  | Data Line 3 (D3) |
| | `GPIO 41` | Đèn nền LCD (PWM Backlight Control) |
| **Cảm Ứng (Touch I2C)** | | **Capacitive Touch Controller (Địa chỉ 0x55)** |
| | `GPIO 38` | I2C SDA (chia sẻ bus với Codec) |
| | `GPIO 39` | I2C SCL (chia sẻ bus với Codec) |
| | `GPIO 48` | Touch Reset (RST) |
| | `GPIO 47` | Touch Interrupt (INT) |
| **Audio Codec ES8311 & Amp** | | **I2S Audio & I2C Control (Địa chỉ 0x18)** |
| | `GPIO 17` | I2S Master Clock (MCLK, 4.096 MHz) |
| | `GPIO 18` | I2S Bit Clock (BCLK) |
| | `GPIO 21` | I2S Word Select / LRCK (WS) |
| | `GPIO 15` | I2S Data Out (ESP32-S3 phát ra DAC ES8311 -> Loa) |
| | `GPIO 16` | I2S Data In (Micro thu vào ADC ES8311 -> ESP32-S3) |
| | `GPIO 1`  | PA Enable (Active LOW: mức 0 Bật amply, mức 1 Tắt) |

---

### 2.1. Bảng Tra Cứu Cấu Hình Thanh Ghi (Hardware Register Maps)

#### A. Everest ES8311 Low-Power Audio Codec (I2C Addr: `0x18`)
Chuỗi khởi tạo thanh ghi chuẩn theo quy trình chính thức (Official ESP-ADF Verified Sequence):

| Địa chỉ Reg | Giá trị Hex | Chức Năng Cấu Hình Chi Tiết |
| :---: | :---: | :--- |
| `0x00` | `0x80` | CSM Enable, Slave Mode (bit 6 = 0: Bit Clock & WS do ESP32 Master cấp). |
| `0x01` | `0x3F` | Kích hoạt toàn bộ xung nhịp nội (Internal Clocks) từ chân MCLK (GPIO 17). |
| `0x02` | `0x00` | Pre-divider = 1, Pre-multiplier = 1. |
| `0x03` | `0x10` | Chế độ fs_mode = 0, tỷ lệ lấy mẫu ADC oversampling (ADC_OSR = 16). |
| `0x04` | `0x10` | Tỷ lệ lấy mẫu DAC oversampling (DAC_OSR = 16). |
| `0x05` | `0x00` | Bộ chia xung nhịp ADC/DAC clock divider = 1. |
| `0x06` | `0x03` | Bộ chia BCLK divider = 4 (tần số BCLK = 4.096MHz / 4 = 1.024MHz cho 16kHz x 32-bit x 2 ch). |
| `0x07` | `0x00` | LRCK Divider High Byte = 0. |
| `0x08` | `0xFF` | LRCK Divider Low Byte = 255 (chu kỳ 256x fs = 4.096 MHz). |
| `0x09` | `0x0C` | SDP IN Format: Định dạng Standard I2S 16-bit, Active / Unmuted. |
| `0x0A` | `0x0C` | SDP OUT Format: Định dạng Standard I2S 16-bit, Active / Unmuted. |
| `0x0B` | `0x00` | System Power Normal. |
| `0x0C` | `0x00` | System Power Normal. |
| `0x0D` | `0x01` | Cấp nguồn cho khối tương tự (Power up Analog Blocks). |
| `0x0E` | `0x02` | Kích hoạt bộ quản lý nguồn hệ thống (System Power Management Enable). |
| `0x10` | `0x1F` | Bật nguồn tầng chuyển đổi tương tự - số ADC (Power up ADC). |
| `0x11` | `0x7F` | Bật nguồn bộ lọc số ADC (Power up ADC Filters). |
| `0x12` | `0x00` | Kích hoạt hệ thống DAC xuất âm thanh. |
| `0x13` | `0x10` | Định tuyến tín hiệu tương tự (System Analog Routing). |
| `0x14` | `0x1A` | **PGA Analog Gain**: Thiết lập mức khuếch đại **+30dB** cho Micro Analog MEMS (bit 6 = 0). |
| `0x15` | `0x40` | Tốc độ tăng dốc ADC ramp rate & Cấp điện áp nuôi Micro **Mic Bias ~2.0V**. |
| `0x16` | `0x24` | Độ nhạy số ADC Digital Gain mặc định. |
| `0x17` | `0xDF` | **ADC Digital Volume**: Bù khuếch đại số **+16dB** giúp thu giọng nói rõ nét, lọc ồn. |
| `0x1B` | `0x0A` | Bộ lọc thông cao ADC High-Pass Filter (HPF Stage 1). |
| `0x1C` | `0x6A` | Bộ cân bằng âm sắc & bộ lọc thông cao ADC Equalizer / HPF Stage 2. |
| `0x31` | `0x00` | Mở tiếng xuất âm DAC (DAC Unmute). |
| `0x32` | `0xBF` | **DAC Digital Volume**: Cài đặt mức **0dB Unity Gain** (chuẩn 0xBF chống vỡ méo tiếng qua amply). |
| `0x37` | `0x48` | **TẦNG CÔNG SUẤT DAC**: Bật tầng xuất âm công suất DAC Output Driver Stage (Bắt buộc). |
| `0x44` | `0x08` | Bản đồ kênh ADC channel mapping & chế độ kiểm tra tín hiệu. |
| `0x45` | `0x00` | General Purpose Control Normal. |
| `0xFD` | `0x83` | Thanh ghi kiểm tra mã nhận diện Chip ID High (`0x83`). |
| `0xFE` | `0x11` | Thanh ghi kiểm tra mã nhận diện Chip ID Low (`0x11`). |

#### B. Bộ Điều Khiển Cảm Ứng Đa Điểm Capacitive Touch (I2C Addr: `0x55`)

| Địa chỉ Reg | Kích Thước | Ý Nghĩa Kỹ Thuật |
| :---: | :---: | :--- |
| `0x0010` | 1 Byte | **Cờ Trạng Thái Chạm (Touch Status)**: Kiểm tra Bit 3 (`info & 0x08`). Nếu bit 3 được set, bộ đệm tọa độ đã sẵn sàng đọc. |
| `0x0014` | 7xN Bytes | **Khối Dữ Liệu Tọa Độ**: Mỗi điểm chạm chiếm 7 bytes liên tiếp:<br>- Byte 0: Bit 7 (`0x80`) cờ nhấn (Pressed), 6 bit thấp là 6 bit cao của X.<br>- Byte 1: 8 bit thấp của tọa độ X.<br>- Byte 2: 6 bit thấp là 6 bit cao của Y.<br>- Byte 3: 8 bit thấp của tọa độ Y. |

#### C. Lệnh Điều Khiển Màn Hình LCD IPS ST7796S / ST77922 (QSPI/SPI)

| Mã Lệnh | Tên Lệnh | Ý Nghĩa Kỹ Thuật |
| :---: | :---: | :--- |
| `0x21` | **INVON** | **Display Inversion On**: Lệnh cốt lõi triệt tiêu hiện tượng âm bản (đảo màu trắng) trên tấm nền IPS, đảm bảo màu nền đen sâu công nghệ Dark Mode. |
| `0x36` | **MADCTL** | **Memory Data Access Control**: Cấu hình hướng quét màn hình ngang (Landscape 480x320) và thứ tự điểm ảnh RGB/BGR. |
| `0x11` | **SLPOUT** | **Sleep Out**: Đánh thức vi mạch điều khiển LCD ra khỏi trạng thái ngủ đông. |
| `0x29` | **DISPON** | **Display On**: Bật hiển thị xuất hình ảnh lên tấm nền tinh thể lỏng. |

---

## 3. Hướng Dẫn Nạp Firmware (Binary Release)

Repository này phát hành trực tiếp các gói nhị phân đã biên dịch sẵn trong thư mục [`firmware/`](firmware/), cho phép bạn nạp và chạy ngay trên mạch mà không cần cài đặt PlatformIO hay tải mã nguồn C++.

Các file nhị phân tương ứng với các phân vùng bộ nhớ Flash 16MB:
* `0x0000`: `firmware/bootloader.bin` (ESP-IDF 2nd stage bootloader)
* `0x8000`: `firmware/partitions.bin` (Partition Table 16MB)
* `0xe000`: `firmware/boot_app0.bin` (OTA Data selection)
* `0x10000`: `firmware/firmware.bin` (Ứng dụng chính Smart Fridge Agent)

---

### Cách 1: Nạp Trực Tiếp Qua Trình Duyệt Web (Khuyến Nghị - Đơn Giản Nhất)
Không cần cài đặt bất kỳ công cụ nào, chỉ cần dùng trình duyệt **Google Chrome** hoặc **Microsoft Edge**:
1. Mở trang web nạp chip ESP: [https://esp.huhn.me/](https://esp.huhn.me/) hoặc [ESP Web Tools](https://espressif.github.io/esptool-js/).
2. Cắm cáp USB Type-C từ máy tính vào bo mạch ESP32-S3 và bấm nút **Connect** để chọn cổng COM.
3. Nhập 4 file từ thư mục `firmware/` với đúng địa chỉ offset sau:
   * Địa chỉ `0x0`: chọn file `bootloader.bin`
   * Địa chỉ `0x8000`: chọn file `partitions.bin`
   * Địa chỉ `0xe000`: chọn file `boot_app0.bin`
   * Địa chỉ `0x10000`: chọn file `firmware.bin`
4. Bấm **Program** để nạp. Quá trình hoàn tất sau khoảng 15-30 giây.

---

### Cách 2: Nạp Nhanh Bằng 1-Click (`flash.bat` / `flash.sh`)
* **Trên Windows**: Cắm cáp USB vào máy tính -> Nhấp đúp chuột chạy file **[`flash.bat`](flash.bat)** -> Nhập tên cổng COM của bạn (ví dụ: `COM8`) -> Nhấn Enter. Tool sẽ tự động nạp toàn bộ firmware.
* **Trên macOS / Linux**: Mở Terminal và chạy:
  ```bash
  chmod +x flash.sh
  ./flash.sh
  ```

---

### Cách 3: Nạp Bằng Dòng Lệnh `esptool`
Nếu bạn đã cài sẵn Python trên máy tính:
```bash
pip install esptool
python -m esptool --chip esp32s3 -b 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 firmware/bootloader.bin 0x8000 firmware/partitions.bin 0xe000 firmware/boot_app0.bin 0x10000 firmware/firmware.bin
```

---

## 4. Hướng Dẫn Vận Hành Thiết Bị

1. **Kết nối mạng**: Sau khi nạp chương trình, chuyển đến **Trang 4 (Cài đặt)** -> bấm **Kết nối Wi-Fi**. Chọn mạng của bạn, nhập mật khẩu trên bàn phím ảo và bấm **Kết nối**. Hệ thống sẽ tự động đồng bộ giờ NTP và lưu cấu hình vào Flash.
2. **Điều chỉnh nhiệt độ**: Tại **Trang 2 (Ngăn lạnh)**, dùng ngón tay kéo thanh slider hoặc bấm `+`/`-` để cài đặt nhiệt độ. Giá trị hiển thị tại đây và Trang chủ sẽ đồng bộ lập tức.
3. **Sử dụng Trợ lý AI**: Chuyển đến **Trang 3 (Trợ lý)**, chạm vào biểu tượng Robot. Đèn báo viền đỏ và hiệu ứng âm thanh kích hoạt trong 2.5 giây. Nói câu hỏi vào micro, hệ thống sẽ tự động gửi âm thanh lên Cloud, nhận diện tiếng Việt và phát câu trả lời qua loa kèm hiển thị trên bong bóng chat.
4. **Chuyển trang linh hoạt**: Bạn có thể bấm trực tiếp vào 4 icon thanh điều hướng bên dưới, hoặc vuốt ngón tay ngang màn hình để lướt qua lại giữa các trang mượt mà.

---

## 5. Bản Quyền & Điều Khoản Sử Dụng
Mọi quyền sở hữu trí tuệ, thiết kế giao diện và thuật toán nhúng thuộc về tác giả. Firmware được cung cấp dưới dạng nhị phân phục vụ mục đích trải nghiệm và thử nghiệm phần cứng. Nghiêm cấm sao chép, dịch ngược mã hoặc thương mại hóa khi chưa có sự đồng ý bằng văn bản của tác giả.
