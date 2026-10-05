# Kiểm tra bản thiết kế — 2026-10-06

Kiểm tra tương tác trực tiếp trên trang HTML/CSS/JavaScript trong trình duyệt
Codex. Đây không phải kiểm thử firmware hoặc ứng dụng iOS native.

## PASS trong trình duyệt

- Ba tab, đường tắt và trạng thái tab đang chọn.
- Phát/tạm dừng, bài tiếp, dừng và chọn bài mẫu; không phát âm thanh thật.
- Slider bằng phím mũi tên cập nhật nhãn từ 60% thành 61%.
- Sheet giao diện, chuyển sáng/tối, chữ lớn, đóng bằng nút hoặc Escape.
- Mẫu chưa kết nối và mất kết nối khóa nút phát; đang kết nối hiển thị
  rõ chưa có dữ liệu/ACK. Chọn thiết bị mẫu phục hồi trạng thái minh họa.
- Chữ lớn ở 375×812, 430×932, 812×375 và khung tablet 768×1024:
  không tràn ngang; đáy vùng nội dung khớp đầu tab bar; các nút trên
  trang Thiết bị được đo có chiều rộng/chiều cao ít nhất 44 CSS px.
- Màn hình ngang 812×375 vẫn cuộn tới bài cuối và chọn được bài;
  hàng cuối nằm phía trên tab bar sau khi cuộn.
- Tương phản các token text/muted/accent/positive/danger trên surface:
  sáng 16.00 / 5.70 / 5.83 / 6.37 / 6.32;
  tối 14.99 / 7.56 / 7.76 / 9.91 / 8.63.
  Đây là phép đo token, không phải chứng nhận accessibility toàn trang.
- Ảnh thực tế từ trình duyệt: `previews/overview-light.jpg`,
  `previews/music-dark.jpg` (375×812, chữ tiêu chuẩn).

## NOT_TESTED / ngoài phạm vi

iPhone thật, Safari iOS, VoiceOver, Dynamic Type, notch/safe area thật,
BLE, âm thanh, điều khiển ESP32. Không thay hoặc nạp firmware.
Các trạng thái, tên bài và âm lượng chỉ phục vụ thiết kế, không xác nhận
thao tác thành công trên thiết bị.
