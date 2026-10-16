# Bước 4 — Engine M2M functional LT

> Milestone M2M. Core peripheral request/clear đã bổ sung ở [bước 5](step_05_peripheral.md).


## Phạm vi và cơ sở

Dùng SystemC 2.3.4/C++17; theo config.md và HDS §2.2, §2.5–2.8,
§2.12, §7, §8.3.2 và completion sequence trang 67–69.
Theo xác nhận của người dùng, transfer unit suy ra từ alignment và tham số,
không thêm thanh ghi/field chọn 8/16/32 bit.

Đây là milestone functional: single-command LAST=1, M2M, không endian swap.
Có 8 channel contexts nhưng scheduler chỉ phát một giao dịch master tại một
thời điểm. Chưa tuyên bố hoàn tất weighted arbitration, read/write overlap hay
4 pending mỗi hướng theo cấu hình phần cứng đích.

## Thuật toán suy ra beat/burst

Tính độc lập tại địa chỉ hiện tại của mỗi endpoint:

1. Lấy limit nhỏ nhất giữa byte còn lại, FIFO capacity/data và BURST_MAX_SIZE.
2. Nếu INCR, giảm limit để không vượt biên 4 KiB.
3. Chọn unit lớn nhất trong 4, 2, 1 byte thỏa alignment địa chỉ và limit.
4. Giới hạn 16 beat; làm tròn tổng byte xuống bội số unit.
5. Endpoint FIXED phát từng beat riêng tại cùng địa chỉ cho target TLM thông thường.

Ví dụ source 0x1000, destination 0x5002, size 7, burst max 8:
read đầu tiên có thể dùng unit=4, sau đó xử lý tail. Write đầu tiên ở địa chỉ
căn 2 nên dùng unit=2. FIFO chỉ lưu chuỗi byte; read/write không cần cùng unit.

Đây là quy tắc functional cụ thể của model, không phải thuật toán đã đối chiếu
RTL từng burst. Không tạo beat 16/32 bit lệch alignment hoặc xuyên word; các buffer
lệch địa chỉ được phân rã thành beat nhỏ có alignment hợp lệ. Khả năng copy một
buffer lệch địa chỉ không có nghĩa hỗ trợ AXI beat unaligned xuyên word.

Metadata `TransferExtension` mang channel, unit, số beat, INCR và ID=4 để test
quan sát. Target RAM không cần hiểu extension vẫn đọc/ghi được dữ liệu liên tiếp.
Đây chưa phải extension AXI đầy đủ (chưa CACHE/PROT hoặc phase signals).

## Luồng thực thi

- Start snapshot source/destination/count vào runtime context; không tự sửa
  command address registers để giả định cách readback chưa được spec định nghĩa.
- Start làm active=3 trước khi callback register trả về, rồi đánh thức engine.
- Engine đọc một burst vào FIFO; ghi dần dữ liệu đó với unit của destination.
- FIFO tối đa 32 byte; status trả free capacity và buffered data chưa gán write.
  Reset suy ra 32 byte free/0 data là policy functional hiện tại, cần đối chiếu RTL.
- Data buffer transaction tách khỏi FIFO để reset trong khi target đang wait
  không làm target ghi đè state mới. Reset epoch loại bỏ kết quả cũ sau transaction.
- Tiêu thụ annotated delay một lần; target cũng được phép wait trong b_transport.
- Completion chỉ sau write response cuối: active=0, command count=1; nếu SET_INT=1
  thì event count=1 và raw bit0=1. Mask/CORE_STATUS/IRQ được cập nhật sau delta cycle.
- Clear bit0 acknowledge event; count command vẫn giữ 1. Chưa có chain nên không
  có trường hợp nhiều completion event đang xếp hàng trong cùng start.
- Global start kiểm tất cả kênh được chọn trước khi start, tránh start nửa nhóm
  nếu một kênh chưa được milestone hỗ trợ. Bit ngoài 7:0 bị bỏ qua.

## Error, disable và chính sách còn mở

- TLM ADDRESS_ERROR ánh xạ READ/WRITE_DECERR; lỗi response khác ánh xạ SLVERR.
  Đây là quy ước adapter functional, không mô phỏng AXI response channel.
- Lỗi bus dừng kênh, đặt raw error, không tăng completion count. Là policy model
  cho milestone vì HDS chưa chốt chi tiết toàn bộ state sau lỗi.
- Disable chặn transaction mới; transaction đã gọi target vẫn hoàn tất.
  Enable lại tiếp tục context. Đây chưa phải abort.
- Khi active, ghi command/static config trả COMMAND_ERROR để giữ context ổn định;
  enable và interrupt mask/clear vẫn truy cập được.
- Start khi đang active/disabled, size=0, burst=0, peripheral endpoint, endian swap
  hoặc LAST=0 bị từ chối bằng COMMAND_ERROR. Các điều kiện chưa được spec định
  nghĩa hoặc chưa triển khai không được coi là hành vi lỗi của RTL.
- Start reset command/event counters và completion raw; sticky error khác vẫn giữ.
  Cần đối chiếu RTL nếu yêu cầu chính xác start/restart semantics.
- Raw status software writes, restriction status và capability chưa đầy đủ vẫn
  bị từ chối; timeout/watchdog và peripheral service chưa thực hiện.

## Kiểm chứng

```bash
cmake -S components/FU3_Components/DMA0/AP_DMA \
  -B /tmp/fu3-ap-dma-step02 \
  -DSYSTEMC_HOME=/home/vinhnt/Documents/deps/systemc-2.3.4 \
  -DBUILD_TESTING=ON
cmake --build /tmp/fu3-ap-dma-step02 --parallel 2
ctest --test-dir /tmp/fu3-ap-dma-step02 --output-on-failure
```

Register và M2M: **2/2 PASS**. M2M kiểm 8 kênh, unit=1/2/4, alignment khác nhau,
byte-for-byte tới 1023 byte, phần dư, biên 4 KiB, 16 beat/FIFO limit, FIXED source
và destination, global start, status/counters, IRQ mask/unmask/W1C, lỗi read/write,
disable/resume, reset trong giao dịch và start lại. Test dùng target có latency.
Chưa xác nhận phần cứng cycle-accurate hoặc performance/outstanding theo RTL.
