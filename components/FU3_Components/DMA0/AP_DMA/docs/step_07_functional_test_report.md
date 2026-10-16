# Bước 7 — Báo cáo regression và đối chiếu chức năng

> Snapshot trước khi tách engines. Chức năng/source và bộ test mới xem [bước 8](step_08_engines_scheduler_chain.md); không dùng coverage bên dưới cho source hiện tại.


Ngày chạy: 2026-10-16 (Asia/Ho_Chi_Minh).

**13/13 test PASS** trên SystemC 2.3.4, GCC 13.3.0, C++17.
Bản bật UndefinedBehaviorSanitizer cũng **13/13 PASS**, không báo undefined behavior
trong các đường chạy đã kiểm. Model production không thay đổi trong bước này;
bổ sung test, tùy chọn gcov và tài liệu/evidence.

Đã kiểm phần model hiện có theo matrix và oracle dữ liệu độc lập. Chưa thể kết
luận full chức năng HDS vì có tính năng chưa triển khai và policy chưa đối chiếu RTL.

## Cách chạy

Từ repository root:

```bash
cd /home/vinhnt/Documents/CDC-VP
bash components/FU3_Components/DMA0/AP_DMA/tests/run_all.sh
```

Script configure, compile rồi chạy toàn bộ CTest. Prefix SystemC mặc định là
`/home/vinhnt/Documents/deps/systemc-2.3.4`, suy ra tương đối từ workspace;
có thể đổi bằng biến môi trường SYSTEMC_HOME. Build mặc định `/tmp/fu3-ap-dma-full`.
JUnit XML ở `<build_dir>/results.xml`; CTest log ở `Testing/Temporary/LastTest.log`.

Chạy riêng và xem output:

```bash
ctest --test-dir /tmp/fu3-ap-dma-full -R '^fu3_ap_dma_matrix$' -V
ctest --test-dir /tmp/fu3-ap-dma-full -R '^fu3_ap_dma_handshake$' -V
ctest --test-dir /tmp/fu3-ap-dma-full -R '^fu3_ap_dma_random_61443$' -V
```

## Bộ test

| CTest | Phạm vi / oracle |
|---|---|
| registers | Reset/RW mask cả 8 kênh, RO/WO policy, compatibility, capability0, reset sau ghi |
| m2m | Byte-for-byte, INCR/FIXED source và destination, group start, IRQ, lỗi, disable/resume |
| peripheral | M2P/P2M FIFO giả; clear sau response và rộng một cycle; delay, software request, error, swap |
| endian | Vector cố định 00/01/10; 60 tổ hợp alignment/burst; group qua nhiều burst; reset giữa partial group |
| matrix | 544 transfer hợp lệ từ 19 kích thước 1..1023, 4×4 alignment, 3 swap mode; burst/latency thay đổi |
| errors | Cả bốn read/write SLVERR/DECERR; IRQ mask/unmask/W1C; retry; no-IRQ completion; active writes; group validation; reset lúc read/write pending |
| decode | Mọi word trong aperture 0x1100 (1088 vị trí); mọi địa chỉ lệch word; null buffer, byte enable, width/length sai, IGNORE và access lúc reset |
| ids | Cả RX và TX, software và external, ID1..31; dữ liệu, auto-clear, rearm; P2P bị từ chối |
| channels | Start đồng thời 8 kênh; source/destination/mode riêng, dữ liệu không lẫn; IRQ bitmap 0xff và acknowledge từng kênh |
| handshake | RX/TX delay 0..7; giữ request cao không nhân credit; request mới đến sớm phải đợi; hai kênh chung ID dùng một credit |
| random_61443 | 600 transfer với seed 61443 |
| random_24301 | 600 transfer với seed 24301 |
| random_20261016 | 600 transfer với seed 20261016 |

Tổng random là 1.800 transfer. Seed cố định để tái hiện lỗi. Matrix/random so
sánh toàn bộ destination với expected bytes từ source snapshot; kiểm guard bytes,
completion và clear IRQ. Tại mỗi master callback kiểm metadata alignment,
16-beat cap, biên 4 KiB, burst cap cấu hình, pending count và reservation FIFO.

Latency gồm zero-delay, annotated delay, target wait và wait kết hợp annotated delay.
Test ghi nhận các chính sách model, không thay thế so sánh waveform/RTL.

## Code coverage

Build gcov:

```bash
FU3_DMA_COVERAGE=ON bash components/FU3_Components/DMA0/AP_DMA/tests/run_all.sh
cd /tmp/fu3-ap-dma-full
gcov -b -c -j CMakeFiles/fu3_ap_dma.dir/src/ap_dma.cpp.gcda
```

Kết quả cho riêng `src/ap_dma.cpp`: **314/315 dòng = 99,68%**;
**418/526 branch directions = 79,47%** được lấy ít nhất một lần.
Dòng chưa chạy là dòng 15, báo fatal cho peripheral_cycle âm. Các nhánh gcov
có cả exception paths do compiler sinh; không phải tất cả là nhánh chức năng.
Số liệu không bao gồm SystemC/STL, không phải phần trăm chức năng spec đã đạt.
Khi cần số lần thực thi chính xác, dùng build dir mới để tránh cộng dồn gcda cũ.

## Đối chiếu 29 nhóm Supported Features của HDS §2

“PASS LT” nghĩa là các test nêu trên pass trong phạm vi functional đã mô tả;
“Partial” chưa đủ để ký xác nhận toàn nhóm; “Chưa có” là code chức năng còn thiếu.

| No. HDS | Nhóm tính năng | Kết quả và giới hạn |
|---|---|---|
| 1 | Core có read/write engine | Partial: một worker tuần tự, chưa hai engine độc lập |
| 2 | 8 DMA channels | PASS LT: channels, matrix/random và isolation |
| 3 | FIFO 32 byte/channel | PASS LT với reservation budget; chưa pin-level FIFO |
| 4 | M2M/M2P/P2M | PASS LT: data oracle, peripheral và ids |
| 5 | APB3 32-bit register path | Partial: register transaction; không PSEL/PENABLE/PREADY |
| 6 | AXI master read/write | Partial: LT payload/metadata; chưa AXI protocol channels, CACHE/PROT mapping |
| 7 | Source/destination config | PASS LT: RW masks và dữ liệu tới địa chỉ riêng |
| 8 | Fixed/increment độc lập | PASS LT trong ca kiểm; không claim burst waveform RTL |
| 9 | Unit 8/16/32 bit | PASS LT theo suy ra alignment/limits đã được người dùng xác nhận |
| 10 | Transfer amount 10 bit | PASS LT 1..1023 và mask; size0 vẫn policy từ chối |
| 11 | Burst tối đa 16 beat | PASS LT metadata và page/burst limits; AXI pin-level chưa kiểm |
| 12 | Per-channel/global start | PASS LT; active/disabled/invalid group response là policy milestone |
| 13 | RX/TX peripheral selection | PASS LT ID1..31 |
| 14 | External requests | PASS LT RX/TX toàn ID1..31, held-level/rearm/shared-ID |
| 15 | Software requests OR external | PASS LT từng nguồn riêng; không claim wrapper đồng thời hai nguồn cùng ID |
| 16 | Peripheral clear pulse | PASS LT rộng một cycle cấu hình, sau response; chưa phase chính xác theo RTL |
| 17 | Delay/request-block control | Partial: delay0..7 và rearm; register request-block chưa được HDS định nghĩa rõ |
| 18 | Independent read/write arbitration | Chưa có: một scheduler tuần tự |
| 19 | Priority/weighted arbitration | Chưa có: token/priority readback chưa điều khiển scheduling |
| 20 | Read/write overlap | Chưa có |
| 21 | Outstanding 1..4 mỗi hướng | Chưa có: functional hiện tối đa một service toàn core; test pending status=1 không chứng minh depth4 |
| 22 | Narrow/partial lanes | Partial: beat aligned 1/2/4, tails mode00/byte payload; chưa WSTRB signal hoặc unaligned RTL |
| 23 | SLVERR/DECERR | PASS LT read/write fault và recovery; mapping TLM response là policy adapter |
| 24 | FIFO overflow/underflow detection | Chưa có đường tạo lỗi/interrupt tương ứng; chỉ kiểm không vượt budget |
| 25 | AXI timeout | Chưa có |
| 26 | Channel watchdog | Chưa có |
| 27 | Interrupt generation | Partial: completion và 4 bus faults; bits5..12, raw-write injection và multiple completion queue chưa kiểm |
| 28 | Runtime status | Partial: active/pending/FIFO/count; restriction status còn thiếu |
| 29 | Capability reporting | Partial: capability0; design capability và capability1 còn thiếu giá trị đã chốt |

Command-chain còn thiếu layout descriptor xác nhận và chưa triển khai fetch/queue.
Endian đã test policy group theo stream; group theo lane và partial final group
chưa chốt. Wrapper DTYPE_SINGLE/BURST/FLUSH, valid/ready acknowledgement và DRLAST
chưa được biểu diễn bởi core bitmap. Không đánh dấu các mục này PASS vì một lệnh
unsupported trả lỗi như dự kiến.

## Evidence và kết luận

Evidence trong `tests/evidence/`:

- `functional_results.xml`, `functional.log`: 13 CTest, lỗi=0.
- `ubsan_results.xml`, `ubsan.log`: cùng 13 CTest với UBSan.
- `coverage.json`: tổng dòng/branch cho source model.

UBSan áp dụng cho model và test executables, không instrument lại thư viện SystemC.
Không phát hiện lỗi trong các ca đã chạy. Còn các khoảng trống chức năng theo bảng
trên, do đó chưa thể gọi đây là signoff toàn bộ IP theo HDS.
