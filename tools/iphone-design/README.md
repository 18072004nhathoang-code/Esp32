# MiniOS — bản thiết kế iPhone

Bản thiết kế tương tác bằng HTML/CSS/JavaScript, không phải app iOS đã biên dịch.
Không thay firmware hoặc trang BLE thật tại `tools/ble-remote`.
Không có Bluetooth, fetch/API, microphone, âm thanh, lưu cấu hình hoặc timer giả.
Tên bài, trạng thái và âm lượng đều là dữ liệu minh họa, có nhãn cố định trong UI.

## Xem bản thiết kế

Từ thư mục repo:

```powershell
python -m http.server 8765 --bind 127.0.0.1 --directory tools/iphone-design
```

Mở `http://127.0.0.1:8765/` trên máy tính. Server chỉ bind localhost;
không tự public hosting hoặc mở firewall. Có thể mở `index.html` trực tiếp trong
trình duyệt vì bản thiết kế không dùng module import hoặc yêu cầu mạng.
Để xem trên iPhone, chuyển cả ba file `index.html`, `design.css`, `prototype.js`
vào một dịch vụ preview do bạn chọn; bản này không triển khai hosting.

## Phạm vi

- Tổng quan: thiết bị, đang nghe, hai đường tắt.
- Âm nhạc: bìa vector, điều khiển mẫu, âm lượng, ba bài minh họa.
- Thiết bị: trạng thái, sheet ghép đôi, thông tin Wi-Fi/SD, giao diện/cỡ chữ.
- Sáng/tối; trạng thái đã nối/chưa nối/đang nối/mất nối.
- Các nút chỉ thay đổi bản xem trước; không giả xác nhận lệnh từ thiết bị.
- Thời gian bài là `—:—`; không có seek, vị trí tự chạy hoặc số đo phần cứng giả.

## Quy tắc thiết kế

Phong cách iOS: font hệ thống, tiêu đề rõ, nhóm nội dung bo góc,
ba tab dưới có nhãn, sheet có nút đóng, vùng chạm ít nhất 44 CSS px cho prototype.
CSS px trong trình duyệt không phải phép kiểm chứng kích thước pt của app native.
Thông tin không phụ thuộc màu; nút icon có nhãn, focus ring rõ; không khóa zoom.
Nội dung được cuộn độc lập phía trên tab bar, có safe-area inset cho điện thoại.
Văn bản UI dùng em, có chế độ chữ lớn; đây không phải kiểm thử Dynamic Type iOS.
Motion nhẹ và chỉ chạy khi không yêu cầu reduced motion.

Không dùng landing-page pattern hoặc font trang trí từ kết quả tìm kiếm skill
vì không khớp sản phẩm điều khiển native. Áp dụng các nguyên tắc đã kiểm tra về
touch target, safe area, native navigation và trạng thái minh họa rõ ràng.

## Giới hạn

Kết nối BLE, VoiceOver/Dynamic Type trên iPhone thật và render Safari/iOS
**NOT_TESTED**. Mục tiêu hiện tại chỉ là bản thiết kế, không phải triển khai kết nối.
Không suy ra app iOS/BLE chạy được từ ảnh prototype.

## Ảnh và kiểm tra

Ảnh chụp từ trình duyệt: `previews/overview-light.jpg` và
`previews/music-dark.jpg`. Kết quả tương tác, responsive và giới hạn
kiểm chứng được ghi trong `VERIFICATION.md`.
