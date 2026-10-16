# DMA0 / AP_DMA — cấu hình chuẩn bị TLM VP

Đã rà/sửa functional model, bổ sung capability/status, AXI metadata, phase timers
và peripheral adapter. **33/33 test functional PASS**, UBSan **33/33 PASS** trên SystemC 2.3.4.
Xem [bước 9](docs/step_09_review_and_fixes.md) cho kết quả, cách chạy và giới hạn.
SystemC prefix: `/home/vinhnt/Documents/deps/systemc-2.3.4`.
Các phần sau là tóm tắt spec/lộ trình lịch sử; trạng thái hiện tại theo bước9.

Đã đọc `FTEL_CDC_DMA_Hardware_Design_Specification_V1.0.docx.pdf`.
File `config.md` là cấu hình tham chiếu dạng Markdown, chưa phải
model SystemC hay file runtime của một simulator hiện hữu. Giá trị `null` nghĩa
là spec chưa chốt; không được tự chuyển thành 0. Mọi offset là tương đối với DMA_BASE.

## Cấu hình lấy từ spec

| Hạng mục | Cấu hình | Nguồn (trang PDF) |
|---|---|---|
| Kênh / FIFO | 8 kênh, 32 byte/kênh | 12–13, 32 |
| Control | APB3, 32 bit | 16 |
| Data master | Địa chỉ/data 32 bit, ID 4 bit, ID cố định 4 | 17–18 |
| Burst | FIXED/INCR, tối đa 16 beat | 18, 32 |
| Pending | Tối đa 4 read + 4 write toàn core; endpoint peripheral chỉ 1 credit | 15–16, 32, 53 |
| Transfer | Byte count 10 bit; giá trị khác 0 tối đa 1023 byte; unit 1/2/4 byte | 19, 40 |
| Arbitration | Weighted round-robin, read/write độc lập | 21 |
| IRQ | Một output tổng hợp, 13 nguồn/kênh | 25, 46–48 |
| Reset | Active-low; sync/async chưa rõ | 33 |
| Register map | Kênh n: `0x100*n`; global `0x1000..0x10ff` | 34–46 |
| Aperture tối thiểu | `0x1100` byte; `0x0800..0x0fff` không ánh xạ | 39 |
| Capability0 | `0x0a602258` | 37–38 |
| DMA0 peripheral slots | 1 SPI0, 2 USART0, 3 USART1, 4 QSPI | 22 |

`config.md` chứa đủ 23 vị trí thanh ghi cho mỗi kênh và 11 thanh ghi global,
quyền truy cập, reset, trường bit và bảng interrupt. Reset đặc biệt:
`CH_ENABLE=1`, `CH_INTERRUPT_ENABLE=0x1fff`, read/write config `0x84010000`.
Các ô không ánh xạ trả slave error, kể cả lỗ trong từng bank.

## Những điểm cần xác nhận trước khi đóng model

1. Bìa V1.0, header FU2 V1.1, thư mục FU3: cần xác nhận đúng phiên bản áp dụng.
2. Overview ghi AXI4, §2.5 nêu WID/LEN 4 bit mang kiểu AXI3. TLM functional vẫn
   xây được; chưa nên tuyên bố tương thích AXI pin-level.
3. Spec có unit 8/16/32 bit nhưng không chỉ ra field chọn unit. Không tự thêm bit.
4. §2.7 còn để mở ý nghĩa transfer size, nhưng bảng thanh ghi và ví dụ §8 ghi byte.
   Config ưu tiên bảng/ví dụ; hành vi size=0 chưa được xác định.
5. §4 còn nội dung cũ (1–8 kênh, 256 units, abort), trái với phần tính năng hiện hành.
   Config dùng 8 kênh cố định, byte count 10 bit; không thêm lệnh abort.
6. §7 có dòng “Total = 0x00000F1F” dưới read config trái với layout bit và reset.
   Config giữ các field theo bảng và §8.3.1, không dùng dòng Total làm mask.
7. Priority register được mô tả nhưng §2.9 chỉ hỗ trợ weighted arbitration.
   Readback/mức ảnh hưởng cần đối chiếu RTL.
8. Chưa có layout đầy đủ command trong RAM, cách chọn transfer unit, threshold
   timeout/watchdog, full capability1/design capability, clock/base/IRQ routing.
   Danh sách còn lại nằm trong “Điểm cần xác nhận” của config.md.

## Đề xuất triển khai

- Bước 1: C++17/SystemC TLM-2.0 LT, register target và data master riêng;
  reset/IRQ, 8 context, FIFO riêng. Kiểm reset, mask, lỗ decode, global start,
  M2M byte-for-byte, completion và W1C. Little-endian/word access là đề xuất model.
- Bước 2: M2P/P2M, request/clear, delay, endian swap, chain sau khi chốt layout.
  DMA0 slot peripheral lấy từ bảng trên; peripheral ID 0 luôn là memory ready.
- Bước 3: Read/write scheduler độc lập, FIFO reservation, pending limit, arbitration,
  lỗi bus, reset trong giao dịch, disable rồi drain. Dùng concurrent contexts hoặc AT
  để kiểm pending thực; không suy ra overlap từ một vòng b_transport tuần tự.

Các burst bị giới hạn bởi byte còn lại, alignment, 16 beat, biên 4 KiB,
FIFO reservation và peripheral credit. Completion chỉ đến sau khi mọi write response
đã xong. Disable chặn grant mới, không hủy giao dịch đã accept. Clear bit 0 chỉ
acknowledge một completion event; IRQ còn giữ nếu còn event.

RX clear sau beat đọc cuối, TX clear sau write response; xung một chu kỳ.
Wrapper adapter giữ một credit; FLUSH acknowledge không chuyển dữ liệu, DRLAST
không kết thúc transfer (§2.10). Có thể tách adapter thành module riêng trong VP.

Không hỗ trợ circular/double buffer, 2D stride, WRAP, exclusive, scatter-gather
ngoài command-chain hiện tại, hoặc unaligned xuyên word tùy ý. Model LT không tự
kiểm chứng RID/BID/RLAST hay timeout từng AXI phase nếu không có adapter/extension.

Component `components/FX1_Components/dma` có register map rất gần và có thể tham khảo,
nhưng không mặc định lấy policy/giả định FX1 làm yêu cầu của FU3. Model `dma_tlm`
PL330 có tập lệnh/register map khác, không dùng thay trực tiếp.
