# Bước 1 — Kiến trúc model DMA0 / AP_DMA

> Tài liệu milestone trước. Engine M2M hiện đã triển khai; xem [bước 4](step_04_m2m.md). Người dùng đã xác nhận unit suy ra từ alignment và tham số, không cần chọn unit cố định.


Trạng thái: thiết kế chuẩn bị triển khai, chưa có code thực thi.
Nguồn: [config.md](../config.md) và HDS trong thư mục IP.
Số trang bên dưới là số trang PDF. Nếu config và HDS mâu thuẫn, ghi lại để
đối chiếu; không dùng lựa chọn của model để thay yêu cầu phần cứng.

## Toolchain đã chốt

Dùng **SystemC 2.3.4**, TLM-2.0 và C++17 theo yêu cầu người dùng.
Các bước CMake/build tiếp theo phải tìm và kiểm tra đúng phiên bản SystemC này.
Đường dẫn cài đặt và compiler/ABI của thư viện sẽ được kiểm tra ở bước dựng khung.

## 1. Model chịu trách nhiệm gì?

DMA là IP tự chuyển dữ liệu sau khi CPU cấu hình và start. CPU không phải đọc/ghi
thay DMA từng byte. APB là đường điều khiển; AXI là đường chuyển dữ liệu.

```text
CPU / register testbench
          |
          v
    target_socket          reset_n
          |                   |
  +-------v-------------------v----------------------+
  | DMA register decode + global registers          |
  |                                                 |
  | 8 channel contexts + 8 FIFO (32 byte/FIFO)       |
  |                                                 |
  | Read scheduler             Write scheduler      |
  |        |                         |              |
  |        +---- data master --------+              |
  | IRQ aggregation              Peripheral service |
  +----|--------------|-------------------|---------+
       v              v                   v
      irq       master_socket      request / clear
                      |                   |
                      v                   v
                 Bus / RAM          Peripherals
```

Các khối bên trong có thể là class/helper trong cùng sc_module, chưa bắt buộc
mỗi khối là một sc_module. Chia như vậy giúp giữ read/write độc lập ngay từ thiết kế.
Nguồn: HDS §2.1–2.5, §2.9–2.11, §4, trang 12–22 và 27–29.

## 2. Giao tiếp công khai dự kiến

Các tên và kiểu C++ sau là đề xuất biểu diễn TLM, không phải tên port RTL.

| Thành phần | Kiểu dự kiến | Chức năng / nguồn |
|---|---|---|
| Module | `fu3::dma::ApDma : sc_core::sc_module` | Một instance DMA core |
| `target_socket` | `tlm_utils::simple_target_socket<ApDma>` | APB register path; §2.4 |
| `master_socket` | `tlm_utils::simple_initiator_socket<ApDma>` | Data read/write; §2.5 |
| `reset_n` | `sc_core::sc_in<bool>` | Reset active-low; §5.2 |
| `irq` | `sc_core::sc_out<bool>` | Một IRQ tổng hợp; §2.15 |
| `rx_request`, `tx_request` | `sc_core::sc_in<sc_dt::sc_uint<32>>` | Bitmap request, bit 1..31; §2.10, §7 |
| `rx_clear`, `tx_clear` | `sc_core::sc_out<sc_dt::sc_uint<32>>` | Bitmap clear; bit 0 không dùng |

Một socket master mang cả READ và WRITE qua loại lệnh transaction, không có
nghĩa là chỉ có một engine hoặc read/write bắt buộc tuần tự.
Bitmap dùng 32 bit để bit p ứng với peripheral ID p; bit 0 dành cho memory,
không cần request ngoài. Peripheral ports nằm trong kiến trúc đích; milestone
M2M đầu tiên chưa kiểm chứng hành vi handshake.

Đây là giao tiếp core request/clear. Adapter wrapper có request type,
acknowledgement và FLUSH cần được mô hình riêng khi làm peripheral (§2.10).
Không tuyên bố bitmap này thay thế đầy đủ adapter wrapper.

Clock frequency và reset sync/async chưa rõ. Chưa chốt sc_in<bool> clk hay
tham số cycle; quyết định ở bước timing sau khi xác nhận thông tin integration.

## 3. Quy ước địa chỉ

- Register target nhận offset tương đối với DMA_BASE; bus/platform trừ base.
  Đây là hợp đồng tích hợp đề xuất, tránh hard-code base trong IP.
- Channel n: `0x100*n + register_offset`, n từ 0 đến 7.
- Global offsets đã gồm `0x1000`, không cộng thêm lần nữa.
- Aperture ít nhất `0x1100` byte, không phải 4 KiB.
- Vùng `0x0800..0x0fff` và mọi lỗ decode trong bank trả slave error.
- Địa chỉ nguồn/đích đi qua master là địa chỉ hệ thống 32 bit, không trừ DMA_BASE.

Nguồn: §7, trang 34–46. Ánh xạ slave error sang
`TLM_ADDRESS_ERROR_RESPONSE` là đề xuất TLM; target RAM cũng phải báo lỗi khi
truy cập sai địa chỉ. Chưa gộp mọi lỗi TLM thành SLVERR hoặc DECERR khi chưa có
quy ước bus. Access thanh ghi 4 byte/little-endian là đề xuất trong config,
không suy ra byte-order của toàn SoC từ data width APB.

## 4. Trạng thái cần quản lý

| Nhóm | Dữ liệu cần giữ | Lý do |
|---|---|---|
| Register bank mỗi kênh | 23 vị trí theo config, tách RW/RO/WO | CPU quan sát và điều khiển đúng register map |
| Transfer context | Địa chỉ hiện tại, byte còn lại riêng read/write, active riêng từng hướng | Hai hướng tiến triển độc lập |
| FIFO mỗi kênh | 32 byte, lượng dữ liệu hợp lệ, read reservation và write allocation | Tránh vượt dung lượng khi có pending |
| Pending | Count read/write mỗi kênh và toàn core | Giới hạn 4 giao dịch mỗi hướng toàn core |
| Completion | Command count và số completion event chưa xử lý | Clear bit 0 chỉ acknowledge một event |
| Interrupt | Raw, enable, masked mỗi kênh | Tổng hợp CORE_STATUS và IRQ |
| Global | Start bitmap, software requests và capability | Giao diện chung cho 8 kênh |

RO status nên suy ra từ runtime state tương ứng, tránh giữ hai bản giá trị bị lệch.
Raw masked IRQ mỗi kênh bằng `raw & enable & 0x1fff`; bit n của CORE_STATUS
bằng 1 khi masked IRQ kênh n khác 0. IRQ tổng bằng OR của các bit đó.
Nguồn: §2.2, §2.11, §2.15 và §7, trang 13, 22, 25, 34–48.

## 5. Luồng M2M đầu tiên

Dùng ví dụ HDS §8.3.2, trang 54–56; các địa chỉ RAM dưới đây là ví dụ testbench,
không phải memory map đã chốt của FU3.

1. Testbench tạo RAM chứa nguồn tại `0x1000` và vùng đích tại `0x5000`.
2. CPU/testbench disable channel 0, bảo đảm không còn outstanding.
3. Clear interrupt cũ bằng `0x1fff`; cấu hình interrupt enable.
4. Ghi source `0x1000`, destination `0x5000`, transfer size `256` byte.
5. Ghi command control `3`: SET_INT=1, LAST=1; không fetch next command.
6. Ghi read/write config `0x81010008`, mode=0, peripheral config=0.
7. Enable rồi ghi start. Enable tự nó không bắt đầu transfer.
8. Engine đọc nguồn vào FIFO rồi ghi sang đích theo giới hạn burst/FIFO.
9. Chỉ báo completion sau khi mọi data write và response hoàn tất.
10. Testbench so sánh 256 byte, kiểm tra trạng thái hoàn tất và IRQ; clear event.

Ví dụ này không giải quyết cách chọn transfer unit 1/2/4 byte còn thiếu trong spec.
Quy tắc đã được người dùng xác nhận ở bước 4; không thêm register bit.
Milestone dựng khung và register/reset vẫn làm độc lập được.

## 6. Điều kiện kiểm chứng các bước tiếp theo

| Milestone | Điều kiện đạt |
|---|---|
| Khung module | Build/link SystemC; bind được target/master/reset/IRQ; chưa claim DMA hoạt động |
| Register/reset | Kiểm reset đã rõ, decode 8 kênh/global, mask và reserved error; không điền 0 cho reset chưa chốt |
| M2M | Byte-for-byte đúng, không vượt FIFO, enable không tự start, completion/IRQ/W1C đúng |
| Peripheral | Request gating, một credit, RX/TX clear đúng thời điểm, mapping DMA0 slots đúng |
| Timing/pending | Read/write overlap thực; count không vượt global 4 và giới hạn endpoint; disable drain; reset khi đang truyền |

Không dùng một vòng b_transport tuần tự để tuyên bố đã kiểm chứng 4 pending.
Không tuyên bố kiểm AXI RID/BID/RLAST hay timeout từng phase nếu chưa có
adapter/extension thể hiện các thông tin đó.

## 7. Điểm để mở

Giữ nguyên danh sách “Điểm cần xác nhận” trong config.md: revision FU2/FU3,
AXI3/4, transfer-unit selection, zero size/burst/token, command-chain layout,
capability chưa đầy đủ, clock/reset/timeout, priority, AXI attributes và các
hành vi truy cập chưa mô tả. Chưa tái sử dụng policy của model FX1 làm yêu cầu FU3.

Bước 1 hoàn tất khi đã có bản phân chia trách nhiệm, giao tiếp và trạng thái như
trên. Bước 2 tạo khung C++/CMake/testbench; chưa chạy copy dữ liệu.
