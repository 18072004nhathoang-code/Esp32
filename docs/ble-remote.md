# Điều khiển Mini OS bằng điện thoại qua BLE

Chức năng A: phát/tạm dừng/tiếp tục/dừng nhạc SD, bài trước/sau, âm lượng và đọc
trạng thái thật. Không truyền âm thanh điện thoại, không A2DP, không đổi WiFi,
không gửi mật khẩu WiFi hoặc key AI qua BLE. Màn hình, touch, pinout và partition
giữ nguyên. Driver dùng Bluedroid/GATT có sẵn trong Arduino-ESP32 2.0.17 / IDF
4.4.7 đã pinned; không thêm thư viện hoặc sửa SDK.

## Trên ESP32

1. Sau khi nạp bản firmware có BLE, mở **Settings**, cuộn xuống
   **Điều khiển nhạc qua BLE**, bấm **Bật Bluetooth BLE**.
2. Điện thoại tìm tên `MiniOS-xxxxxx` hiển thị trên ESP32. WiFi/hotspot vẫn kết nối
   như trước; không cần đổi mạng để điều khiển bằng BLE.
3. Kết nối và đọc Status / đăng ký Notify. Nhập mã 6 chữ số hiện trên màn hình
   Settings khi hệ điều hành điện thoại yêu cầu. Không dùng mã hard-code.
4. Chỉ Secure Connections + MITM + khóa 16 byte được chấp nhận. Không dùng
   Just Works, không fallback không mã hóa. Không lưu bonding/passkey vào NVS;
   mỗi kết nối mới phải ghép đôi lại. Mã không được log lên Serial/backend.
5. Bấm **Tắt Bluetooth BLE** để đóng kết nối và giải phóng host/controller.
   Chuyển app không ngắt BLE; mất BLE không tự dừng/phát nhạc.

BLE mặc định **tắt sau boot**, không lưu trạng thái bật vào NVS. Khởi tạo chỉ tạo
worker 4096 byte + queue giới hạn; radio/host chỉ cấp phát khi người dùng bật.
Nếu thiếu SRAM, UI báo lỗi và rollback, không giả kết nối thành công. Hãy dừng
nhạc và đóng AI rồi bật lại. Controller chỉ cấp phát 2 air activities (advertising
+ một kết nối), thay vì mặc định SDK 6. Ngưỡng trước init: internal heap ≥72 KiB,
largest block ≥32 KiB; sau init vẫn giữ ≥32 KiB / ≥12 KiB cho audio/WiFi. Đây là guard tài
nguyên, không phải chứng minh đã stress-test BLE + WiFi + Xiaozhi. Lỗi teardown
thử tối đa 3 lần, sau đó yêu cầu restart nếu tài nguyên chưa được nhả. Không
retry init vô hạn. Pairing không hoàn thành trong 60 giây sẽ đóng peer.

DMA màn hình dùng strip 8 dòng (3840 byte), thay strip 40 dòng (19200 byte),
giải phóng 15360 byte SRAM. Flush vẫn đồng bộ với DMA; driver/pixel RGB565,
240x320 rotation 2 và tọa độ touch không đổi. Cần kiểm tra độ mượt trên bo thật.

## Điện thoại

### Trang điều khiển (trình duyệt Web Bluetooth)

Các file `tools/ble-remote/index.html` + `remote.mjs` là client không dependency,
không cần provider key hoặc device token. Backend hiện có phục vụ **`/ble/`** qua
allowlist file cố định. Chạy backend như hiện tại và mở **`https://<backend>/ble/`**
trên trình duyệt có Web Bluetooth; chọn thiết bị bằng nút **Kết nối ESP32**.
Không tự triển khai public hosting/billing. HTTPS cần chứng chỉ hợp lệ như phần
cấu hình backend hiện có; HTTP qua địa chỉ LAN của PC/điện thoại không đáp ứng
secure context của Web Bluetooth. Không thay endpoint Xiaozhi/firmware.

Browser không có Web Bluetooth (bao gồm Safari iPhone) phải dùng ứng dụng BLE
GATT; trang hiển thị lý do, không mô phỏng kết nối. Trang chỉ báo lệnh thành công
khi Status chứa ID đúng và kết quả `Applied`. ATT Write response chỉ xác nhận
nhận lệnh, không phải nhạc đã phát. Timeout không gửi lại lệnh tự động; đọc trạng
thái để biết kết quả thực tế. Ngắt kết nối/hủy chooser làm vô hiệu callback cũ.

### Ứng dụng BLE GATT (Android/iPhone)

Trong ứng dụng BLE GATT (ví dụ nRF Connect for Mobile), scan và Connect tới
`MiniOS-xxxxxx`; không tìm mục “loa Bluetooth” của hệ điều hành. Giữ Settings
ESP32 mở để đọc mã ghép đôi. Trong service bên dưới, bật Notify của Status rồi
Write **with response** ở Command. Một lệnh outstanding/connection, ID tăng dần
từ 1 đến 65535; hết ID thì disconnect/reconnect. Không gửi URL, JSON hay audio.

Chỉ ghép đôi trong **Cài đặt Bluetooth Android** chưa gửi được lệnh nhạc. Trong
nRF Connect, mở tab **CLIENT**, chạm **Unknown Service** có UUID `7cf10000-…`
để mở rộng. Đọc characteristic `7cf10002-…` và bật Notify ở đó; gửi đúng 6 byte
dạng **Byte array/HEX** tới `7cf10001-…`, không gửi chuỗi chữ ASCII. MiniOS không
cần quyền truy cập danh bạ hoặc nhật ký cuộc gọi trên điện thoại.

Kết quả `Status` chỉ hoàn tất lệnh đọc trạng thái, không phải ACK cho Play,
Pause, Resume, Stop hoặc âm lượng. Client chờ `Applied` / mã lỗi cho những lệnh
này; không phát lại lệnh tự động khi nhận Status hoặc hết thời gian chờ.

| UUID | Vai trò |
| --- | --- |
| `7cf10000-6e6d-4f73-9f2e-455333433238` | Service |
| `7cf10001-6e6d-4f73-9f2e-455333433238` | Command, Write with response, authenticated |
| `7cf10002-6e6d-4f73-9f2e-455333433238` | Status, Read + Notify, authenticated |

Command đúng **6 byte**: `[version=1, op, id_LE16, value_LE16]`.

| Lệnh | op | value | Ví dụ HEX với ID riêng |
| --- | --- | --- | --- |
| Phát bài SD đầu tiên | 1 | index 0-based | `01 01 01 00 00 00` |
| Tạm dừng | 2 | 0 | `01 02 02 00 00 00` |
| Tiếp tục | 3 | 0 | `01 03 03 00 00 00` |
| Dừng | 4 | 0 | `01 04 04 00 00 00` |
| Âm lượng 50% | 5 | 0–100 | `01 05 05 00 32 00` |
| Trạng thái | 6 | 0 | `01 06 06 00 00 00` |
| Bài sau | 7 | 0 | `01 07 07 00 00 00` |
| Bài trước | 8 | 0 | `01 08 08 00 00 00` |

Status luôn **20 byte**, dùng được MTU 23. Notify tối đa 1 lần/giây, thêm phản hồi
sau lệnh; đọc lại khi notification thất lạc/congested.

| Offset | Ý nghĩa |
| --- | --- |
| 0 | Version 1 |
| 1 | Flags: playing=1, paused=2, WiFi=4, SD=8, AI busy=16, snapshot unavailable=32 |
| 2–3 | ID lệnh gần nhất đã xử lý, LE16; 0 khi chưa có |
| 4 | Result: 0=status, 1=Applied (decoder ACK), 2=Invalid, 3=Busy, 4=NotConfirmed, 5=Unavailable |
| 5 | Âm lượng 0–100 |
| 6 | Index bài signed 8-bit; -1 là nguồn Internet, không phải index SD |
| 7 | Số bài SD (tối đa 64 trong firmware) |
| 8–11 | Vị trí giây LE32 |
| 12–15 | Thời lượng giây LE32; 0=decoder chưa xác định |
| 16–19 | Session generation LE32; client bỏ notification khác session hiện tại |

Snapshot unavailable là lỗi khóa/service, không được coi các giá trị 0 là số đo.
Ở version 1, bit flags 6–7 phải bằng 0, số bài SD không vượt 64 và index nằm
trong -1…63. Client từ chối Status ngoài contract và ngắt kết nối, không dùng
notification lỗi để xác nhận lệnh nhạc đã thành công. Không tự gửi lại lệnh.
Trong lúc AI có generation thu/phát/cleanup hoặc trạng thái STARTING…CANCELING,
BLE trả Busy, không acquire I2S. Resume sau Pause + hỏi AI dùng bookmark revision
đã có; Stop/đổi source làm bookmark cũ vô hiệu. Lệnh nhạc đi qua mutex/queue/ACK
của Music Player, không chạy decoder trong callback Bluetooth/LVGL.

Lệnh trùng hoặc ID cũ bị từ chối, queue đầy không cập nhật state/success. Prepared
write/offset không hỗ trợ bị từ chối. Disconnect/tắt BLE tăng generation: lệnh
chưa bắt đầu và kết quả cũ bị bỏ. Nếu disconnect khi decoder đang thực thi lệnh
đã nhận, lệnh đó có thể đã áp dụng; không thể suy ra nó đã hủy chỉ vì mất BLE.

## Kiểm chứng

- C++ native chạy **cùng** `src/connectivity/ble_remote_logic.cpp` và `Gate` như
  firmware: parser/MTU packet, nguồn SD, busy, ACK failure, ID trùng, queue rollback,
  disconnect/reopen không nhận command/result cũ.
  Fixture wire `tests/fixtures/ble_status_v1.txt` dùng chung với parser điện thoại.
- `node --test tests/ble_remote_web_test.mjs`: protocol phone, ACK, timeout, cancellation.
- `scripts/run_lvgl_layout_test.py`: chạy layout card Settings dùng cùng code
  production, LVGL 8.3.11 và font Be Vietnam Pro 12/14; kiểm tra text/status/mã
  ghép đôi/lỗi không chồng lên nút. Card tự tăng chiều cao, cuộn ở Settings bên
  ngoài; không giảm font hoặc tạo scroll riêng trong card. Native CI dùng
  ASan/UBSan; trên Windows dùng compiler hỗ trợ với UBSan nếu không có ASan.
- `npm test --prefix backend`: route tĩnh allowlist/HEAD/traversal và tests backend.
- `scripts/run_native_tests.sh` + `pio run -e esp32-s3-es3c28p`.
- Driver radio, pairing Android/iPhone, audio BLE+WiFi coexist, bật/tắt 100 lần,
  heap/stack/I2S qua lượt hỏi thật: **NOT_TESTED cho đến khi thử trên bo và điện thoại**.

Không coi build/mock PASS là điện thoại đã kết nối thành công.

Tham chiếu driver: [Espressif GATT IDF 4.4.7](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/bluetooth/esp_gatts.html)
và [GAP/security](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/bluetooth/esp_gap_ble.html).
Client: [Chrome Web Bluetooth](https://developer.chrome.com/docs/capabilities/bluetooth),
[nRF Connect for Mobile của Nordic](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-mobile).
