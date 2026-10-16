# Bước 8 — Read/write engines, scheduler, pending, timers và command-chain

> Snapshot bước8. Các lỗi và phần thiếu đã xử lý tiếp ở [bước9](step_09_review_and_fixes.md); quantum=0, capability và phase-timeout behavior bên dưới thuộc source cũ.


## Kết quả triển khai

Model có hai hướng dispatch độc lập, mỗi hướng 4 worker context gọi b_transport.
Hai arbiter WRR riêng chọn kênh cho read và write. Đây là concurrent blocking TLM,
không phải AXI pin-level hoặc giao thức AT nb_transport. Target bus/RAM phải cho
phép các lời gọi b_transport reentrant từ nhiều SystemC process.

HDS liên quan: §2.5, §2.8–2.11, §2.13, §7 và §8.3.7–8.3.8.
Người dùng đã xác nhận layout command-chain 4 word little-endian trong bước này.
Không thêm thanh ghi MMIO hay field chọn transfer unit.

## Luồng thực thi

```text
Register start
     |
     v
Scheduler: retire -> timers -> command completion -> issue read / issue write
     |                                         |
 Read WRR                                  Write WRR
     |                                         |
 4 read contexts                           4 write contexts
     |                                         |
     +---------- master_socket ----------------+
                     |
                 Bus / RAM
```

- Scheduler xét điều kiện riêng mỗi hướng, nên read không chờ write response để
  cấp read tiếp nếu còn FIFO/pending budget, và ngược lại.
- Read issue giữ chỗ FIFO trước khi phát transaction. Read response được retire
  theo thứ tự issue rồi đi qua endian grouping và vào FIFO.
- Write issue lấy byte FIFO vào buffer thuộc context. Buffer này vẫn được tính
  trong budget 32 byte đến khi write response retire. Đây là policy bảo thủ, không
  quảng cáo thêm capacity ngoài FIFO spec.
- Vì external ID luôn là 4, queue retire riêng read/write giữ thứ tự issue ngay
  cả khi các lời gọi target trả về lệch thứ tự. Không sửa ngược được side effect
  mà một target reentrant đã thực hiện ngoài thứ tự; target chịu trách nhiệm
  ordering vật lý nếu integration cần điều đó.
- CH_OUTSTANDING_STATUS đếm logical context từ dispatch tới retirement. Đây là
  tương đương TLM của accepted transactions, không phải quan sát AR/AW pins.
- FIFO status read free = 32 - buffered - partial swap - read reservations -
  write reservations. Write unassigned = buffered bytes chưa cấp cho context.
- Completion chỉ sau tất cả read/write bytes, contexts và FIFO của command đã drain.

## Pending limits

Giới hạn toàn core: **4 read + 4 write**, không phải 4 × 8 cho mỗi hướng.
Với mỗi endpoint:

| Điều kiện | Limit |
|---|---|
| Endpoint peripheral | 1 |
| Memory, OUTSTANDING_ENABLE=0 | 1 |
| Memory, OUTSTANDING_ENABLE=1 | Clamp OUTS_MAX vào 1..4 |

OUTS_MAX và enable độc lập ở CH_READ_CONFIG/CH_WRITE_CONFIG.
FIFO budget, request credit và channel enable có thể làm depth thực thấp hơn limit.
FIXED service nhiều beat vẫn chỉ một logical context; không tự tăng pending theo
số fragment b_transport. M2M FIXED giữ cách phát từng beat của milestone trước.

## Scheduler và WRR

TOKENS[21:16] là số grant liên tiếp tối đa trước khi nhường arbiter cho kênh khác.
Read/write có current/next/token counter riêng. Kênh không eligible không giữ bus.
Token=0 dùng một grant là policy model cho trường hợp HDS chưa định nghĩa rõ.
CORE_PRIORITY_CONFIG vẫn chỉ lưu/readback, không bật fixed-priority trái với
HDS §2.9 weighted-only.

Khi hết work eligible, scheduler ngủ đến request/register đổi, worker xong hoặc
deadline gần nhất. scheduler_wakeups() là counter quan sát test, không phải MMIO.
EngineConfig.scheduler_quantum=0 dùng delta-cycle khi còn tiến triển; nếu cần
thời gian dispatch dương, platform truyền quantum. Đây không phải tần số RTL.
Đặc biệt chain lặp vô hạn với target zero-delay cần quantum dương để simulation
time tiếp tục tiến; model không tuyên bố hỗ trợ circular-buffer mode phần cứng.

## Timer integration

```cpp
fu3::dma::EngineConfig config;
config.transaction_timeout = sc_core::sc_time(1, sc_core::SC_US);
config.watchdog_timeout = sc_core::sc_time(10, sc_core::SC_US);
config.scheduler_quantum = sc_core::sc_time(1, sc_core::SC_NS);
fu3::dma::ApDma dma{"dma", sc_core::sc_time(10, sc_core::SC_NS), config};
```

Các con số là ví dụ integration, không lấy từ HDS. Mặc định hai timer bằng 0,
tức tắt. Peripheral cycle vẫn do platform truyền khi dùng M2P/P2M.

- Transaction timer đo từ dispatch context đến khi target hoàn tất cả blocking
  wait và annotated delay. Quá hạn read đặt raw bit7 (read-data timeout), write
  đặt bit9 (write-response timeout), ngừng grant mới cho kênh và drain context cũ.
- Target trả về đúng ngưỡng được coi thành công. Quá hạn được xét ở threshold
  cộng một simulation time-resolution tick để không phụ thuộc thứ tự process
  khi response và deadline có cùng timestamp.
- Watchdog đo từ start hoặc lần retire tiến triển gần nhất của kênh, bao gồm
  chờ peripheral request. Hết hạn đặt raw bit12. Disable tạm dừng watchdog;
  enable lại mở interval mới. Transaction đã accept vẫn chịu transaction timer.
- Timeout không hủy được một callback đang block trong external target. Pending
  và buffer còn giữ đến khi callback trả về. Nếu target không bao giờ trả về,
  IRQ vẫn báo timeout nhưng context không thể tái sử dụng; cần recovery ở target.
- Không phát clear peripheral cho service fault/timeout, không tạo completion
  thành công cho command bị lỗi. Những transaction đã accept khác vẫn drain.

LT chưa phân biệt AR/AW/W/R/B phases, do đó không tự phát bit8/10/11 cho address/
write-data timeout. Muốn kiểm chính xác các phase này cần bus adapter/extension;
không giả lập bằng các timer trùng nhau để tuyên bố đạt spec.

## Command-chain đã chốt

CH_CMD_CONTROL bit1 LAST=0: sau khi command hiện tại hoàn tất, engine fetch tại
`CH_CMD_CONTROL & ~3u`. Mỗi descriptor 16 byte:

| Byte offset | Word little-endian |
|---|---|
| +0 | Source address |
| +4 | Destination address |
| +8 | Byte count, chỉ dùng [9:0] |
| +12 | Command control: SET_INT, LAST và next pointer |

Static read/write/mode/peripheral/attribute config giữ nguyên qua chain.
Descriptor fetch dùng read context với metadata command_fetch=true; split tại
4 KiB, không endian-swap descriptor. Payload mới chỉ issue khi descriptor hoàn tất
và validate hợp lệ. Current command registers cập nhật sang descriptor vừa load.

Mỗi command hoàn tất tăng completed counter 12 bit (wrap). Command có SET_INT
thêm một completion event; counter 4 bit saturate tại 15 là policy model.
W1C bit0 dequeue một event; IRQ giữ khi còn event. Start mới reset counters theo
milestone trước, nên software phải xử lý event trước khi start lại.
Chain dừng tại LAST=1; EngineConfig.command_chain=false có thể tắt hỗ trợ chain.
Descriptor lỗi bus, size0, endian mode reserved hoặc partial group bị lỗi read
DECERR; completion của các command trước vẫn giữ để software xử lý.

## Reset và lỗi đồng thời

Mỗi job giữ epoch và buffer riêng. Reset xóa register/runtime của epoch hiện tại,
nhưng không tái sử dụng slot của callback cũ đang block. Khi nó trả về, scheduler
bỏ kết quả cũ và trả slot về pool. CORE_IDLE_STATUS còn báo busy nếu vẫn có slot
vật lý cũ đang drain, dù pending architectural sau reset bằng 0.
Không rollback side effect đã thực hiện ở RAM/peripheral bên ngoài.

Lỗi bus/timeout/watchdog đặt fault, chặn cả hai hướng cấp grant mới của kênh.
Pending đã accept không bị xóa sớm; channel active hạ khi các context drain hết.
Các kênh khác vẫn được xét theo budget/order chung.

## Kiểm chứng

Bộ cũ 13 CTest giữ data/IRQ/reset oracle; bổ sung test_engines.cpp với 10 case:

| Case | Điều kiện kiểm |
|---|---|
| pending | Quantum=1ns; đạt 4 read và 4 write; cùng hoạt động 4+4 với nhiều kênh; dữ liệu đúng |
| limits | Clamp0/1/2/3/4/15; enable mỗi hướng; peripheral vẫn một credit |
| ordered | Target trả read thứ hai trước read đầu; write chờ head retire, data đúng |
| arbitration | Grant đầu 3:1 riêng read và write; scheduler ngủ khi chờ request |
| timeout | Quantum=100ns lớn hơn threshold30ns nhưng timer vẫn đúng hạn; read/write timeout trước khi target trả; pending giữ tới drain; retry |
| watchdog | Chờ request, disable/resume interval và tiến triển đều không timeout |
| chain | Nhiều command, fetch xuyên4KiB, event queue/W1C, fetch lỗi, descriptor lỗi, saturation |
| reset_pending | Reset khi bốn callback cũ còn block; epoch mới chạy đúng sau khi có slot |
| timer_boundary | Response đúng threshold không false timeout/watchdog |
| fault_drain | Lỗi khi nhiều context đang chờ; chặn grant mới, drain rồi retry |

Build và chạy:

```bash
FU3_DMA_BUILD_DIR=/tmp/fu3-ap-dma-engines \
  bash components/FU3_Components/DMA0/AP_DMA/tests/run_all.sh
ctest --test-dir /tmp/fu3-ap-dma-engines -L engines -V
```

Kết quả functional: **23/23 PASS** trên SystemC 2.3.4; bản UBSan cũng **23/23 PASS**, không báo undefined behavior trong đường chạy đã kiểm. Thư viện SystemC không instrument lại.

Coverage mới cho ap_dma.cpp: **435/437 dòng (99.54%)**; branch directions **774/1002 (77.25%)**. Đây là coverage source hiện có, không phải conformance toàn HDS.
Evidence hiện tại nằm trong tests/evidence/step08/. Số coverage/13-test ở bước7
là snapshot cũ, không dùng để đánh giá source đã đổi trong bước8.

## Giới hạn còn lại

Full AXI/APB pins và wrapper DTYPE/FLUSH/DRLAST, phase-specific timeouts,
CACHE/PROT bus metadata, đầy đủ capability/restriction status và FIFO error
injection chưa hoàn tất. Endian partial-group policy vẫn như bước6.
Có concurrent contexts không đồng nghĩa cycle-accurate hoặc đo throughput RTL.
