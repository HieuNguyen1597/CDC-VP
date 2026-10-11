# H.264 Inter Prediction

Author: VinhNT124

Mô hình dự đoán liên khung cho H.264, gồm core C++17 và lớp giao tiếp SystemC 2.3.4/TLM-2.0. Module thực hiện integer/fractional motion estimation, motion compensation và tạo metadata cho motion syntax từ dữ liệu current macroblock và reconstructed reference.

## Chức năng

- IME (Integer Motion Estimation): tính SAD trên 16 block 4×4 cho mỗi candidate, tổng hợp 41 vị trí thuộc 7 loại partition.
- FME (Fractional Motion Estimation): lấy integer winner làm điểm khởi đầu, đánh giá fractional offsets do bên gọi cung cấp; nội suy luma bằng bộ lọc six-tap và quarter-sample averaging.
- MC (Motion Compensation): xuất predictor Y/U/V theo thứ tự transform block, kèm metadata plane/block/phase/last.
- EEI: tạo thông tin reference, partition, MV và MVD từ MVP do bên gọi cung cấp.
- Reference cache riêng cho List0 và List1, quản lý picture tag, vùng pixel đã nạp và mở rộng biên ảnh.
- Giữ sample/metadata ổn định khi downstream chờ; quản lý token, reset và shared epoch qua lớp TLM.

Cost dùng accumulator 64 bit và từ chối overflow. Bên gọi cung cấp thứ tự candidate, rate cost và MVP; khi total cost bằng nhau, module giữ candidate được xét trước. Nội suy giữ intermediate có dấu trước khi clip kết quả.

## Cấu trúc và CMake targets

Public namespace: `h264::inter`.

| Target | Vai trò |
|---|---|
| `h264_inter_core` | Reference cache, SAD, search, interpolation, MC và motion syntax; không phụ thuộc SystemC |
| `h264_inter_tlm` | Socket TLM, timing và điều khiển Evaluate/Commit/Peek/Accept |

Các alias tương ứng là `cdc::components::h264_inter_core` và `cdc::components::h264_inter_tlm`. Header công khai nằm trong `include/h264/inter/`, implementation trong `src/`, test riêng trong `tests/`. Lớp TLM sử dụng shared contracts từ `../interfaces` và `../synchronization`; SW DMA integration tests dùng sibling module `../dma_subsystems`.

## Luồng dữ liệu và core API

```text
Reconstructed reference ── SW DMA / refill ──> Reference cache
Current MB + candidates ── Evaluate ────────> IME / FME
Selected decision       ── Commit ─────────> MC
Downstream              <── Peek / Accept ── Predictor + metadata
```

[inter_core.h](include/h264/inter/inter_core.h) cung cấp API không phụ thuộc SystemC. Request nhận current luma macroblock Y16×16, tọa độ macroblock tuyệt đối và một partition. P-picture dùng List0; B-picture yêu cầu integer candidates và dữ liệu search hợp lệ trên cả List0/List1. Kết quả chọn uni-prediction trên một list. Fractional offsets tương đối với integer winner, trong khoảng [-3,3] quarter sample.

Reference tag gồm picture identity và REFM slot 0..2. Mỗi lần `retag` tạo một incarnation mới, kể cả khi tag không đổi. `fill` kiểm tra toàn vùng trước khi cập nhật trạng thái đã nạp; pixel đã nạp giữ nguyên đến lần retag tiếp theo, và vùng overlap được chấp nhận khi dữ liệu giống nhau.

Hỗ trợ progressive 8-bit planar YUV 4:2:0. Coded width/height trong khoảng 16..4096, chia hết cho 16; khi kết nối với DMA, kích thước tối đa dùng chung là 1920×1088. Motion vector giới hạn ±65536 quarter sample.

## Giao tiếp TLM

[inter_tlm.h](include/h264/inter/inter_tlm.h) định nghĩa các operation của socket mô phỏng:

| Address / operation | Command / payload | Dữ liệu và kết quả |
|---|---|---|
| `0` / Evaluate | WRITE, 256 byte Y và request extension | Trả decision, token và EEI; chưa commit |
| `1` / Commit | WRITE, 0 byte và token | Khóa winner để MC tạo predictor |
| `2` / Peek | READ, 1 byte và token | Trả sample/metadata hiện tại; chưa dịch chuyển cursor |
| `3` / Accept | WRITE, sample vừa Peek và token/accept_sequence | Xác nhận sample và chuyển cursor; sample cuối đặt done |

Peek có thể lặp lại và trả cùng sample. `output()` giữ data/metadata khi downstream chờ; chỉ Accept mới tiến tới sample tiếp theo. Chroma của partition nhỏ có thể chỉ chứa một phần của transform block 4×4; adapter TQ cần ghép đủ các phần trước khi transform.

Adapter SW DMA gọi `retag` trước search và `refill` sau khi read thành công. Tag khớp nhưng thiếu vùng reference cần thiết vẫn làm search chờ dữ liệu. Retag trong search hoặc retag reference đang được chọn làm mất hiệu lực của quyết định liên quan. Inter nhận dữ liệu qua adapter; việc phát giao dịch DMA do module SW đảm nhiệm.

Tạo module/socket trước `sc_start` và gọi `b_transport` từ `SC_THREAD`. Giữ payload, buffer và extension tồn tại đến response. Module xử lý một transport và một mode pending tại một thời điểm; request/source được copy trước incoming-delay wait. `progress()` cho biết stage, candidate và sample đang xử lý.

### Timing và reset

Các latency trong options là tham số timing VP. `reset_n` active-low hủy công việc; cạnh xuống enable và cạnh lên bắt đầu frame mới vô hiệu hóa cả hai reference lists và mode. Sau reset khi enable đang cao, đưa enable xuống thấp rồi lên cao để bắt đầu lại.

Có thể gắn `h264::EpochExtension` để dùng shared reset domain. Epoch đã stale tại entry bị từ chối mà vẫn giữ công việc hợp lệ đang có; epoch bị hủy trong một giao dịch đang chạy làm mất hiệu lực của frame đó. Bên gọi giữ domain tồn tại đến response và phối hợp enable/reset để bắt đầu lại, kể cả khi shared reset xảy ra lúc module idle.

## Build và test riêng

Yêu cầu CMake từ 3.21 và compiler C++17. Các lệnh dưới đây chạy từ thư mục gốc `CDC-VP` trong WSL/Linux.

### Core không dùng SystemC

```sh
cmake -S components/h264/Inter-Prediction -B build/h264-inter-core \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DH264_INTER_BUILD_TLM=OFF -DH264_INTER_BUILD_DMA_TESTS=OFF
cmake --build build/h264-inter-core -j 4
ctest --test-dir build/h264-inter-core --output-on-failure
```

### Core, SystemC/TLM và SW DMA tests

SystemC 2.3.4 phải tương thích compiler và C++ ABI với project. Thay đường dẫn dưới đây bằng vị trí cài đặt trên máy:

```sh
cmake -S components/h264/Inter-Prediction -B build/h264-inter-tlm \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DH264_INTER_BUILD_TLM=ON -DH264_INTER_BUILD_DMA_TESTS=ON \
  -DSYSTEMC_INCLUDE_DIR=/opt/systemc-2.3.4/include \
  -DSYSTEMC_LIBRARY=/opt/systemc-2.3.4/lib/libsystemc.so
cmake --build build/h264-inter-tlm -j 4
ctest --test-dir build/h264-inter-tlm --output-on-failure
```

Có thể dùng `CMAKE_PREFIX_PATH` trỏ đến package `SystemCLanguage` 2.3.4 hoặc `SYSTEMC_SOURCE_DIR` thay cho cặp include/library. Tắt `H264_INTER_BUILD_DMA_TESTS` khi chỉ cần core và TLM mà không chạy SW DMA tests.

| Nhóm test | Số test | Phạm vi |
|---|---:|---|
| Core | 3 | SAD, cache, search, MC/EEI, interpolation và geometry/MV limits |
| SW DMA integration | 3 | Reference Y/U/V, lỗi, retry, retag và overlap trên bus 32/64/128-bit |
| TLM contract | 12 | Timing thường/zero latency, reset/disable, token, retag, partition và deadline boundaries |

Build đầy đủ đăng ký **18 test**; tắt SW DMA tests còn **15 test**; tắt cả TLM và SW DMA tests còn **3 test**. Kết quả kiểm chứng và logs được lưu trong [VALIDATION.md](VALIDATION.md); các kịch bản được mô tả trong [TEST_PLAN.md](docs/TEST_PLAN.md).

## Test tích hợp H.264

[Prediction_Test](../tests/Prediction_Test/README.md) nối SW DMA với Inter qua `H264Arb` và `DmaTransport`, sử dụng arbiter/AXI bridge của DMA subsystem. Nhóm `sw_inter` kiểm tra reference, predictor, metadata, Peek và 384 mẫu Y/U/V của một macroblock trên bus 32/64/128-bit. Nhóm control kiểm tra phối hợp Intra/Inter trong workload.

[Full codec test](../tests/Coding_Reconstruction_Test/FULL_PIPELINE_CODEC_REPORT.md) nối Inter với TQ/ITQ, CAVLC/NAL, reconstruction và reference DMA. Test adapter xử lý ảnh 32×32 YUV420, bốn frame I-P-P-P, một List0 reference và P16×16 với zero MV. Kiểm chứng codec của nhóm này giới hạn trong cấu hình trên.

```sh
cmake -S components/h264 -B build/h264-vp \
  -DCMAKE_BUILD_TYPE=Debug -DH264_BUILD_TESTS=ON \
  -DSYSTEMC_INCLUDE_DIR=/opt/systemc-2.3.4/include \
  -DSYSTEMC_LIBRARY=/opt/systemc-2.3.4/lib/libsystemc.so
cmake --build build/h264-vp -j 4
ctest --test-dir build/h264-vp -R '^h264_prediction_sw_inter_' --output-on-failure
ctest --test-dir build/h264-vp -L prediction --output-on-failure
ctest --test-dir build/h264-vp -R '^full_codec_' --output-on-failure
```

Label `prediction` gồm **15 test tích hợp** cho DMA, Intra, Inter và control. Component H.264 import các library prediction với suite riêng bị tắt, nên cần build standalone ở trên để chạy đủ test của Inter. Nhóm **36 test `full_codec_*`** chỉ được đăng ký khi CMake tìm thấy Python3 và FFmpeg; kiểm tra danh sách bằng `ctest --test-dir build/h264-vp -N -R '^full_codec_'`.

## Phạm vi sử dụng

Bên gọi chọn partition, candidate, rate cost và MVP, đồng thời cung cấp reconstructed reference qua SW DMA. TQ, reconstruction, entropy và deblocking được thực hiện bởi các module khác. Automatic partition/mode selection, skip/direct và bi/weighted prediction chưa được triển khai.

Timing VP chưa được đối chiếu cycle với RTL. Pipeline codec hiện nằm trong test adapter; Inter chưa được đưa vào install/export của package H.264.
