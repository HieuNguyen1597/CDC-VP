# Bước 5 — Peripheral request/clear cho M2P và P2M

> Endian swap đã bổ sung ở [bước 6](step_06_endian.md). Các giới hạn khác bên dưới vẫn giữ nguyên.


## Mục tiêu và nguồn

Bổ sung đường core request/clear theo HDS §2.3.2–2.3.3 (trang 15–16),
§2.10 (trang 21–22), thanh ghi peripheral config/software request (§7),
và trình tự M2P/P2M/software request (§8.3.3–8.3.5, trang 57–66).
Giữ nguyên engine functional LT một transaction tại một thời điểm.

## Hiểu cách DMA làm việc với peripheral

Memory ID=0 luôn ready. Peripheral ID=1..31 cần request. Phần mềm chọn ID riêng
cho read và write bằng CH_PERIPHERAL_CONFIG, không gắn cố định channel với IP.

- M2P: source là memory; DMA được đọc trước vào FIFO. Khi có TX request đúng ID,
  engine mới ghi FIFO vào địa chỉ peripheral. TX clear phát sau write response cuối
  của service, kể cả khi target dùng wait hoặc annotated delay.
- P2M: DMA chờ RX request đúng ID rồi đọc peripheral. RX clear phát sau read response
  cuối của service. Dữ liệu đã nhận tiếp tục được ghi ra memory mà không cần request
  ngoài. Completion IRQ chỉ sau khi ghi memory hoàn tất.
- Một external request giữ cao chỉ cấp một service. Producer phải hạ request
  xuống rồi mới đưa request tiếp theo. Hạ request không hủy service đã dispatch.
- Request software OR với external tại bước xét eligibility. Sau service thành công,
  bit software tương ứng tự clear. Không dùng software/external cùng ID đồng thời
  nếu wrapper integration chưa xác nhận, theo cảnh báo của HDS trang 66.

Ví dụ USART0 RX (DMA0 slot 2): peripheral config RD_PERIPH_NUM=2,
WR_PERIPH_NUM=0. Chọn fixed source, read burst max=1 byte theo ví dụ HDS trang 65.
Mỗi RX request đưa một byte từ data register vào FIFO; memory write tiến địa chỉ.

## Một request chuyển bao nhiêu dữ liệu?

Một request cho phép một service bị giới hạn bởi burst config, byte còn lại,
FIFO và giới hạn beat. Không mặc định một request luôn chuyển đúng một byte.
Ví dụ fixed peripheral config burst max=8, địa chỉ căn 4: engine có thể thực hiện
hai beat 4 byte cho cùng service và phát một clear sau beat thứ hai hoàn tất.

Để tương thích target TLM thông thường, FIXED service được tách thành các
b_transport một beat cùng địa chỉ. Giữ reservation/pending cho service đến hết
các fragment, không phát clear cho từng fragment. Việc gộp này là lựa chọn
functional, chưa mô phỏng AR/AW handshake của burst FIXED thật.

## Tham số thời gian mới

```cpp
fu3::dma::ApDma dma{"dma", sc_core::sc_time(10, sc_core::SC_NS)};
```

Tham số thứ hai là `peripheral_cycle`: chu kỳ DMA do platform cung cấp.
10 ns là giá trị của testbench, không phải tần số lấy từ spec.
Nếu không truyền, mặc định là SC_ZERO_TIME: M2M vẫn chạy; start M2P/P2M trả lỗi
để không tự đặt một chu kỳ không có trong integration.

RX/TX clear dài đúng một peripheral_cycle. RD/WR_PERIPH_DELAY làm endpoint
chờ N chu kỳ kể từ service hoàn tất trước khi service tiếp theo. Trong thời gian
clear còn cao, cùng ID chưa được cấp credit mới dù software request vừa ghi lại.
Delay/rearm/pulse là state riêng; engine không bận polling ở mỗi cycle khi chờ request.
Chỉ đánh thức theo thay đổi request/register hoặc deadline gần nhất.

Đây là timing tương đối của model; không claim đồng bộ với cạnh clock RTL.
Điểm bắt đầu recovery là policy ở cuối service; cần đối chiếu RTL để khớp cycle.

## FIFO và lỗi/reset

FIFO status trừ read reservation khỏi free bytes, trừ write reservation khỏi
received bytes chưa gán write. Không báo phần dữ liệu đang gửi là dữ liệu mới
có thể cấp cho một write khác.

Clear chỉ phát khi service thành công. Lỗi bus dừng kênh, báo error, không clear
request và không tạo completion thành công; wrapper/peripheral cần recovery.
Các fragment đã target chấp nhận có thể đã gây side effect, không rollback.
Reset xóa request bookkeeping, software requests, clear deadlines và active state;
reset epoch bỏ kết quả trả về muộn. Target/peripheral bên ngoài cần reset riêng
nếu muốn xóa side effect đã xảy ra.

## Kiểm chứng

`test_fu3_ap_dma_peripheral` dùng RAM và peripheral FIFO giả tại các địa chỉ test
0x9000 (RX), 0xa000 (TX), không phải memory map FU3 chính thức. Peripheral target
có wait(2 ns) và annotated delay 1 ns để kiểm service chờ đủ response.

Kiểm: chờ đúng ID, không tái dùng request giữ cao, M2P burst nhiều beat/credit,
P2M byte stream, clear sau response và rộng 10 ns, RX recovery 3 chu kỳ,
software RX/TX auto-clear và ID31, error không phát clear, disable/resume,
reset khi đang chờ request. M2M và register regression giữ nguyên.

Build bằng lệnh trong bước 4; CTest **3/3 PASS** trên SystemC 2.3.4.

## Phần chưa triển khai

- Adapter wrapper có DTYPE_SINGLE/BURST/FLUSH, valid/ready và DRLAST.
  Core bitmap không mang đủ metadata để tự mô phỏng các tín hiệu này.
- Peripheral-to-peripheral bị từ chối (không thuộc ba direction được hỗ trợ).
- Endian swap, command-chain, weighted arbitration, overlap/multiple pending,
  watchdog/AXI phase timeout và full capability chưa có.
- Register raw-write policy và các mục chưa chốt trong config vẫn để mở.
