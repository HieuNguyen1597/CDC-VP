# Bước 9 — Rà soát, sửa và mở rộng kiểm chứng functional TLM

Ngày: 2026-10-16. SystemC 2.3.4, GCC 13.3.0, C++17.
Tài liệu này thay thế các danh sách “chưa có” ở bước 7/8 cho source hiện tại.

## Các lỗi thời gian đã sửa

1. MMIO có annotated arrival delay trước đây cập nhật ngay. b_transport hiện
   chờ đến arrival, cập nhật thanh ghi rồi trả delay=0; không tiêu thụ hai lần.
2. Scheduler trước đây có thể dispatch sớm khi event ngắt thời gian quantum.
   Hiện dùng next_dispatch_ làm điều kiện độc lập, nên event hoàn tất chỉ đánh
   thức để retire, không bỏ qua thời điểm grant kế tiếp.
3. Chain vòng với target zero-delay có thể giữ scheduler ở cùng timestamp.
   Quantum=0 hiện dùng một simulation-resolution tick tối thiểu giữa các batch
   dispatch, cho phép thread firmware/reset ở thời điểm tương lai tiếp tục chạy.
   Đây là simulation granularity, không phải tần số RTL được suy đoán.

Guard FIFO overflow cũng được siết lại: nếu guard phát hiện lỗi khi retire read,
không phát peripheral clear và không ghi nhận progress thành công cho service đó.

## Các phần bổ sung

| Phần | Hành vi hiện tại |
|---|---|
| DESIGN_CAPABILITY_STATUS | Mặc định 1 IRQ destination; Config.design_capability cho integration bổ sung upper bits |
| CORE_CAPABILITY_STATUS1 | Mặc định 0x0f17: watchdog, timeout, token, outstanding, independent, peripheral, chain, endian; chain bit tắt nếu config.command_chain=false |
| CH_RESTRICTION_STATUS | Reset0; cập nhật khi cấu hình địa chỉ/endpoint/pending hoặc load command; bits5/6 độc lập từng hướng |
| Raw interrupt writes | W1S/OR, mask0x1fff; không tạo completed-command count; W1C vẫn dequeue một event |
| CACHE/PROT | Memory/peripheral defaults từ EngineConfig; payload override từ CH_AXI_ATTR_REG; descriptor dùng memory defaults |
| WSTRB metadata | Một mask/beat theo lane/unit; INCR tiến lane, FIXED giữ lane |
| Protocol/FIFO fault | Adapter có thể báo response ID và BusFault; core ánh xạ sang IRQ, chặn grant mới và drain |
| Phase timeout | Timer riêng ReadAddress/ReadData/WriteAddress/WriteData/WriteResponse, tương ứng IRQ8/7/11/10/9 |
| PeripheralAdapter | Một credit, SINGLE/BURST metadata, FLUSH không chuyển payload, ack valid/ready, DRLAST chỉ là metadata |

Capability bits mô tả tính năng đã có trong model; timer threshold=0 nghĩa là
đang tắt timer, không xóa capability hỗ trợ timer. DESIGN upper bits mặc định0
và điều kiện full-FIFO permission=32-byte alignment là policy integration/model,
chưa được so khớp với RTL cấu hình thực. Không gọi giá trị suy ra này là golden RTL.

## Cấu hình và nối phase-aware target

```cpp
fu3::dma::EngineConfig config;
config.memory_read_cache = 3;
config.memory_write_cache = 6;
config.memory_read_prot = 5;
config.memory_write_prot = 2;
config.transaction_timeout = sc_core::sc_time(1, sc_core::SC_US);
config.watchdog_timeout = sc_core::sc_time(10, sc_core::SC_US);
config.phase_timeouts[static_cast<unsigned>(fu3::dma::BusPhase::WriteData)] =
    sc_core::sc_time(100, sc_core::SC_NS);
fu3::dma::ApDma dma{"dma", sc_core::sc_time(10, sc_core::SC_NS), config};
```

Số trong ví dụ là tham số integration. Trong callback target có thể:

```cpp
auto* bus = payload.get_extension<fu3::dma::TransferExtension>();
if (bus) bus->enter_phase(fu3::dma::BusPhase::WriteData);
// wait để biểu diễn phase này; rồi enter_phase(WriteResponse) khi chuyển phase.
```

Adapter dùng extension đang sống của lời gọi b_transport. enter_phase cập nhật
mốc thời gian và đánh thức scheduler. Target chỉ cần read/write RAM thông thường
có thể bỏ qua extension; khi đó model chỉ biết phase mặc định ReadData/WriteResponse.
Không suy ra AR/AW/W handshake thật từ một annotated delay của RAM.
Clone extension không giữ con trỏ event của engine, tránh callback vào context cũ.

response_id_valid/response_id là quan sát RID/BID tùy chọn, so với ID cố định4.
BusFault mang báo cáo overflow/underflow, RLAST, fetch-credit, WSTRB và multiple
FIFO returns từ adapter. Test inject các quan sát đó để kiểm software-visible
error handling, không chứng minh một adapter RTL đã tự phát hiện protocol fault.
Các guard FIFO nội bộ vẫn bảo vệ ngân sách 32 byte và truy cập buffer.

## Peripheral adapter

include/fu3/dma/peripheral_adapter.h là module functional có clock/reset,
request_valid/ready/kind, DRLAST, ack_valid/ready và đường DMA request/clear.
Nối một adapter cho mỗi endpoint, pack request vào bit peripheral ID và unpack
clear từ bit đó. Clock adapter và peripheral_cycle core phải dùng cùng chu kỳ.

RequestKind Single=0, Burst=1, Flush=2 là encoding API model, chưa phải mapping
RTL DTYPE vì HDS không định nghĩa các mã bit. Ack kind/last là metadata quan sát.
Kind3 reserved không được accept. SINGLE/BURST cùng giữ một credit tới DMA clear;
không tự thêm burst credit hoặc điều khiển kết thúc command bằng DRLAST.
FLUSH trả ack mà không phát core request. Ack bị backpressure sẽ giữ valid và
metadata; không accept request mới trước khi ack cũ được nhận.

Reset functional active-low assertion tức thời; các handshake khác xét theo
posedge clock. Cần đối chiếu reset sync/async với RTL thật trước pin-level signoff.
Khi service fault không có clear, adapter chờ tới recovery/reset như HDS §8.3.8.

## Kiểm chứng mới

Giữ 23 CTest cũ và thêm 10 test, tổng **33 test**:

| Case mới | Nội dung |
|---|---|
| capabilities | Capability defaults/integration, restriction từng hướng, inject từng IRQ0..12 trên cả 8 kênh, mask/W1C/W1S |
| attributes | Memory/peripheral default và override, descriptor không nhận payload override, WSTRB/beat, extension clone |
| protocol_faults | Sai RID/BID và 6 loại fault report trên read/write; IRQ chính xác, drain và retry |
| phase_timers | Cả 5 phase timeout, báo trước khi callback trả, pending giữ tới drain, phase transitions |
| mixed_chain | Endian và SET_INT xen kẽ; P2M chain byte credits; disable/reset giữa fetch; 4097 command kiểm wrap12bit và saturate15 event |
| stress | 30 batch × 8 kênh random pending/burst/token, write response lệch thứ tự; WRR token0/63, TX OUTS_MAX4 vẫn single-credit |
| boundaries | MMIO annotated arrival; chain vòng zero-delay vẫn cho phép reset ở thời điểm tương lai |
| timers_combined | Transaction/watchdog cùng hạn, healthy channel tiếp tục khi kênh khác chờ request, deadline cũ không tác động epoch mới |
| adapter_rx | SINGLE/BURST/FLUSH, ack backpressure, DRLAST ignored, dữ liệu RX qua adapter, reset/reserved-kind |
| adapter_tx | Cùng giao thức adapter cho TX và dữ liệu ghi peripheral |

Watchdog sau reset: slot có callback cũ được giữ tới khi target trả. Command mới
có thể tự hết watchdog nếu thời gian chờ slot vượt ngưỡng của chính nó; đây không
phải deadline cũ rò sang epoch mới. Test kiểm cả trường hợp chờ ngắn thành công
và chờ lâu hết watchdog. Pending timeout cũng không bị xóa giả để ép PASS.

## Đối chiếu phạm vi với spec

| Nhóm HDS | Đã kiểm ở mức functional | Phần cần xác nhận thêm |
|---|---|---|
| 8 kênh, direction, FIFO, addresses, burst/unit | Data oracle, guards, reservation, page/burst limits và concurrency | Cycle/pin timing RTL |
| APB register path | Decode/mask/reset, RO/WO policy, arrival delay | APB handshake thực và response RO/WO chưa mô tả |
| AXI read/write/attributes/narrow | Payload, context, metadata CACHE/PROT/WSTRB, lỗi response | AR/AW/W/R/B waveforms, adapter protocol detection |
| Software/global start | Từng kênh, nhóm, atomic invalid-group policy | Start active/disabled policy so với RTL |
| Peripheral selection/request/clear/delay | ID1..31, external/software credit, pulse, delay0..7, shared ID | Software+external cùng ID đồng thời cần integration xác nhận |
| Wrapper | Functional SINGLE/BURST/FLUSH, ack handshake, DRLAST metadata | Mã DTYPE/pin naming và reset semantics của wrapper RTL |
| WRR/overlap/pending | Hai arbiter, token0/1/3/63, 4+4, clamp/enable, in-order retirement, stress | Throughput/cycle-level tương đương RTL |
| Error/timeout/watchdog/interrupt | IRQ0..12 qua natural completion, timer hoặc fault/debug injection; drain/reset/retry | Dò lỗi bus/FIFO tại từng phase pin-level |
| Runtime/capability | Các vị trí register đã có read behavior; count wrap/saturation và restriction | Upper design capability, full-FIFO permission, policy overflow cần golden RTL |
| Chain/endian | Layout4word user đã xác nhận, static config, queues và group qua burst/request | Endian grouping theo bus lane và partial final group chưa được HDS chốt |

WRAP, circular/double buffer, 2D stride, exclusive và scatter-gather ngoài command
chain không thuộc tính năng HDS hiện hỗ trợ, nên không thêm implementation giả.
Zero-size/burst start, partial endian group và reserved mode vẫn bị từ chối
trước payload theo policy các bước trước. Raw W1S và token0=1 được ghi rõ là policy.

## Chạy và evidence

```bash
FU3_DMA_BUILD_DIR=/tmp/fu3-ap-dma-review \
  bash components/FU3_Components/DMA0/AP_DMA/tests/run_all.sh
ctest --test-dir /tmp/fu3-ap-dma-review -L review -V
```

Functional: **33/33 PASS**. Evidence mới nằm trong tests/evidence/step09/.
Bản **UBSan: 33/33 PASS**, không báo undefined behavior trong các ca đã chạy.
SystemC library không được instrument lại. Core code coverage: **525/530 dòng
(99,06%)**, branch directions **946/1211 (78,12%)**. Adapter functional:
**22/22 dòng (100%)**, branch directions **79/120 (65,83%)**.
Các branch có cả đường exception của compiler; không phải phần trăm chức năng HDS.
Chưa chạy các constructor fatal paths, guard FIFO invariant bị phá và enum phase
không hợp lệ. FIFO fault reports/debug injection đã kiểm xử lý IRQ, không cố làm
hỏng private state chỉ để tăng coverage.

Evidence gồm XML, log cho cả functional/UBSan/coverage run, coverage.json và
source_sha256.json để nhận diện chính xác source đã test.
Các số coverage ở bước7/8 là snapshot lịch sử, không áp dụng source hiện tại.

Đây là kiểm chứng phần functional theo spec và các policy được nêu rõ. Chưa có
RTL/reference model để kết luận toàn IP tương đương hoặc pin-level signoff.
