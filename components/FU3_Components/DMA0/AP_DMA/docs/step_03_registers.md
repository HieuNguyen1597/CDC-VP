# Bước 3 — Register bank và reset (phần đã rõ trong spec)

> Tài liệu milestone trước. Engine M2M hiện đã triển khai; xem [bước 4](step_04_m2m.md). Người dùng đã xác nhận unit suy ra từ alignment và tham số, không cần chọn unit cố định.


## CPU đọc/ghi như thế nào?

`b_transport` nhận transaction, kiểm tra loại lệnh/kích thước/địa chỉ,
chuyển 4 byte little-endian thành word rồi gọi `access`. Hàm `access` chọn kênh
bằng address/0x100 và offset bằng address%0x100. Mỗi kênh có vùng lưu riêng,
cho nên ghi kênh 0 không thay đổi kênh 1.

Với RW, giá trị lưu bằng `value & mask`. Ví dụ TRANSFER_SIZE chỉ giữ 10 bit:
ghi 0xffffffff thì đọc lại 0x3ff. Đây là mask theo field, chưa phải validation
cấu hình transfer hợp lệ.

## Đã triển khai

- 8 register contexts theo HDS §7, trang 39–48.
- Command address/size/control, read/write config, mode, peripheral và AXI attrs.
- Reset READ/WRITE_CONFIG=0x84010000, ENABLE=1, INTERRUPT_ENABLE=0x1fff.
- Compatibility schedule/joint/clock-divider đọc 0, ghi không có hiệu lực.
- Các status inactive đã rõ, capability0=0x0a602258, core idle=1.
- Priority register lưu/readback 16 bit; không khẳng định ảnh hưởng arbitration.
- Software RX/TX requests lưu bitmap, bit 0 luôn 0. Chưa có service/auto-clear.
- Công thức masked status, CORE_STATUS và IRQ; raw hiện chỉ có giá trị 0.
- Reserved holes trả ADDRESS_ERROR; địa chỉ lớn không bị truncate về 32 bit.
- Reset lần hai khôi phục cả các thanh ghi đã bị test thay đổi.

Nguồn: config.md, HDS §7 và §8.1. Giá trị status inactive chỉ đúng vì chưa có engine.

## Chính sách TLM và phần còn thiếu

| Trường hợp | Hiện tại | Giới hạn |
|---|---|---|
| Access register | 4 byte, aligned, little-endian, không byte enable | Quy ước model, không phải claim toàn bộ APB |
| Reset assertion | Xử lý ở delta cycle sau cạnh xuống reset_n | Chưa quyết định reset đồng bộ với clock RTL |
| Access lúc reset | COMMAND_ERROR | Chính sách model; spec yêu cầu không giao dịch lúc reset |
| RO write / WO read | COMMAND_ERROR | Chưa được HDS quy định response cụ thể |
| START channel/global | COMMAND_ERROR | Engine chưa triển khai; không báo start thành công giả |
| FIFO/restriction status | COMMAND_ERROR | Chưa đủ thông tin reset/điều kiện bit |
| Design capability / capability1 | COMMAND_ERROR | Chưa tự điền giá trị cố định |
| Raw status write | COMMAND_ERROR | Cần xác nhận OR-set hay replacement |
| Software request write | Thay bitmap với bit0 masked | Quy ước RW tạm thời; cần đối chiếu RTL, nhất là write/service đồng thời |
| Interrupt clear | Clear raw bits đang có | Raw chỉ 0; chưa kiểm hàng đợi completion/event count |
| Latency | Không thêm delay | Chưa chốt timing; không phải cycle-accurate |

IRQ cập nhật qua một SC_METHOD duy nhất để tránh nhiều process cùng ghi output.
Chưa có nguồn tạo event: test hiện chỉ kiểm IRQ inactive. Chưa kiểm chứng IRQ
assertion, event queue hoặc W1C khi có nhiều completion event. Request ports và
clear outputs chưa có hành vi peripheral.

## Test và build

Dùng cùng prefix SystemC 2.3.4 đã cài, xem bước 2. Target mới:
`test_fu3_ap_dma_registers`, tên CTest `fu3_ap_dma_registers`.

Regression kiểm cả 8 kênh: known reset, RW masks, channel isolation, inactive
status, reserved holes, unsupported registers, compatibility registers, software
request masks, high-address rejection, malformed payload và reset sau ghi.
Không có transaction master trong milestone này.

Start/M2M đã triển khai ở bước 4 theo transfer-unit rule người dùng xác nhận. Các policy
chưa rõ ở trên phải được xác nhận hoặc cấu hình rõ trước khi claim model đầy đủ.
