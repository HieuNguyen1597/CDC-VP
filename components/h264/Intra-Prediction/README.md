# H.264 Intra Prediction

Author: VinhNT124

Mô hình dự đoán nội khung cho H.264, gồm core C++17 và lớp giao tiếp SystemC 2.3.4/TLM-2.0. Module tạo predictor, residual và quyết định mode cho từng block; các pixel đã tái tạo được lưu làm dữ liệu tham chiếu cho block tiếp theo.

## Chức năng

- Luma 4×4: hỗ trợ đủ 9 mode; luma 16×16: vertical, horizontal, DC và plane.
- Chroma U/V 8×8: hỗ trợ DC, horizontal, vertical và plane.
- Tra cứu các mẫu top, left, upper-left và upper-right từ reconstructed context, với trạng thái hợp lệ riêng cho từng plane.
- Đánh giá mode bằng SAD hoặc SATD Hadamard 4×4, kết hợp `mode_penalty` do bên gọi cung cấp. SATD của block lớn là tổng các tile 4×4; cost dùng accumulator 64 bit.
- Giữ predictor, residual signed 16 bit và reference snapshot theo token đến khi nhận feedback reconstruction.
- Quản lý frame, reset và hủy giao dịch qua shared epoch.

Các mode hợp lệ được xét theo số mode tăng dần. Khi cost bằng nhau, module giữ mode được xét trước. Nếu upper-right không khả dụng, các mẫu E..H được thay bằng D; các vị trí scan 3/7/11/13/15 không đọc upper-right chưa hợp lệ.

## Cấu trúc và CMake targets

Public namespace: `h264::intra`.

| Target | Vai trò |
|---|---|
| `h264_intra_core` | Thuật toán prediction, chọn mode và reconstructed context; không phụ thuộc SystemC |
| `h264_intra_tlm` | Socket TLM, timing, token và vòng đời frame/reset |

Các alias tương ứng là `cdc::components::h264_intra_core` và `cdc::components::h264_intra_tlm`. Header công khai nằm trong `include/h264/intra/`, implementation trong `src/`, test riêng trong `tests/`. Lớp TLM sử dụng shared contracts từ `../interfaces` và `../synchronization`.

## Luồng dữ liệu và cấu hình

```text
CMB DMA / local RAM ── Evaluate(source) ──> IntraTlm
TQ adapter          <── Replay(predictor, residual)
Reconstruction      ── Reconstruct(decoded pixels) ──> neighbor/mode RAM
```

Adapter CMB đóng gói source pixels theo từng block row-major. Sau khi đọc predictor/residual, bên xử lý TQ/ITQ tái tạo và clip pixel về 8 bit rồi gửi lại module. Reference context được cập nhật bằng pixel đã tái tạo thông qua reconstruction feedback hoặc `ImportReconstructed`.

Ví dụ cấu hình và kết nối:

```cpp
h264::intra::Options opts;
opts.coded_width = 176;
opts.coded_height = 144;
h264::intra::IntraTlm intra("intra", opts);
initiator.bind(intra.target_socket);
intra.reset_n(reset_n);
intra.frame_enable(frame_enable);
```

Tạo module và kết nối socket/signals trước `sc_start`. Gọi `b_transport` từ `SC_THREAD`; giữ payload, buffer và extension tồn tại đến khi giao dịch trả về.

Coded width/height cố định cho mỗi instance, trong khoảng 16..4096 và chia hết cho 16. Tọa độ `Block::x/y` tính theo sample của plane; kích thước U/V bằng một nửa Y. Luma 4×4, luma 16×16 và chroma 8×8 phải căn chỉnh lần lượt theo 4, 16 và 8 sample. Khi kết nối với DMA, kích thước tối đa dùng chung là 1920×1088.

Bên gọi xử lý macroblock theo raster order và luma 4×4 theo thứ tự scan:

```text
 0  1  4  5
 2  3  6  7
 8  9 12 13
10 11 14 15
```

## Giao tiếp TLM

Các địa chỉ sau xác định operation của socket mô phỏng. Mỗi giao dịch cần `h264::intra::Extension`, buffer N×N byte, streaming width ít nhất bằng buffer length và không dùng byte-enable. Socket không cấp DMI.

| Address / operation | Command | Dữ liệu và kết quả |
|---|---|---|
| `0` / Evaluate | WRITE | Source pixels, block, metric và penalty; trả decision/token |
| `1` / Replay | READ | Token của Evaluate; trả predictor và decision chứa residual |
| `2` / Reconstruct | WRITE | Pixel đã tái tạo cùng token; cập nhật neighbor/mode RAM và `block_done` |
| `3` / ImportReconstructed | WRITE | Pixel đã tái tạo và block; nhập context khi không có block pending |

Module giữ tối đa một block pending. Replay có thể đọc nhiều lần với cùng token và luôn trả snapshot đã chọn. Reconstruct yêu cầu đã có Replay; Evaluate hoặc ImportReconstructed khi đang pending bị từ chối mà vẫn giữ block hiện tại. Replay/Reconstruct lấy thông tin block từ token đã lưu.

`ready()` cho biết predictor đang chờ feedback. `block_done()` giữ trạng thái hoàn thành đến Evaluate thành công tiếp theo hoặc frame/reset. Các event `predictor_ready` và `block_completed` dùng để đánh thức bên chờ; bên gọi vẫn cần kiểm tra trạng thái và tổng hợp completion của từng block thành completion của macroblock.

### Timing và reset

Incoming delay được xử lý trước operation. Các latency mặc định cho reference, candidate, compare, replay và feedback lần lượt là 10/4/1/4/10 ns; đây là tham số timing của mô hình VP. Socket xử lý một giao dịch tại một thời điểm; giao dịch đồng thời nhận generic error và có thể thử lại sau khi giao dịch đang chạy trả về.

`reset_n` active-low hủy công việc và context cũ. Cạnh lên `frame_enable` bắt đầu frame mới và xóa context; giữ enable cao không tạo thêm frame. Sau reset, đưa enable xuống thấp rồi lên cao để bắt đầu lại. Token gồm generation và sequence, mất hiệu lực sau frame/reset.

Có thể gắn `h264::EpochExtension` để dùng shared reset domain. Epoch đã stale tại entry bị từ chối mà vẫn giữ block hợp lệ hiện tại; nếu epoch bị hủy trong một giao dịch đang chạy, toàn bộ context frame đó bị vô hiệu hóa. Bên gọi phối hợp reset/frame-enable cả khi socket idle, giữ domain tồn tại đến response và chờ giao dịch cũ trả về (`transport_active()==false`) trước khi tái sử dụng buffer.

## Build và test riêng

Yêu cầu CMake từ 3.21 và compiler C++17. Các lệnh dưới đây chạy từ thư mục gốc `CDC-VP` trong WSL/Linux.

### Core không dùng SystemC

```sh
cmake -S components/h264/Intra-Prediction -B build/h264-intra-core \
  -DCMAKE_BUILD_TYPE=Debug -DH264_INTRA_BUILD_TLM=OFF -DBUILD_TESTING=ON
cmake --build build/h264-intra-core -j 4
ctest --test-dir build/h264-intra-core --output-on-failure
```

### Core và SystemC/TLM

SystemC 2.3.4 phải tương thích compiler và C++ ABI với project. Thay đường dẫn dưới đây bằng vị trí cài đặt trên máy:

```sh
cmake -S components/h264/Intra-Prediction -B build/h264-intra-tlm \
  -DCMAKE_BUILD_TYPE=Debug -DH264_INTRA_BUILD_TLM=ON -DBUILD_TESTING=ON \
  -DSYSTEMC_INCLUDE_DIR=/opt/systemc-2.3.4/include \
  -DSYSTEMC_LIBRARY=/opt/systemc-2.3.4/lib/libsystemc.so
cmake --build build/h264-intra-tlm -j 4
ctest --test-dir build/h264-intra-tlm --output-on-failure
```

Có thể dùng `CMAKE_PREFIX_PATH` trỏ đến package `SystemCLanguage` 2.3.4 hoặc `SYSTEMC_SOURCE_DIR` thay cho cặp include/library.

| Nhóm test | Số test | Phạm vi |
|---|---:|---|
| Core | 2 | Prediction modes, neighbor context, SAD/SATD và regression |
| TLM contract | 1 | Evaluate/Replay/Reconstruct, token và completion |
| Shared epoch | 2 | Hủy giao dịch trong incoming delay và đánh thức wait |
| Reset matrix | 1 | Reset trong các giai đoạn xử lý |
| TLM regression | 2 | Timing tùy chỉnh và zero latency |
| Reset boundaries | 2 | Reset quanh deadline với timing thường và zero latency |

Build core-only đăng ký **2 test**; build có TLM đăng ký **10 test**. Kết quả kiểm chứng và logs được lưu trong [VALIDATION.md](VALIDATION.md).

## Test tích hợp H.264

[Prediction_Test](../tests/Prediction_Test/README.md) nối CMB DMA với Intra qua `H264Arb` và `DmaTransport`, sử dụng arbiter/AXI bridge của DMA subsystem. Nhóm `cmb_intra` kiểm tra một ảnh 16×16 gồm 16 luma 4×4 và hai chroma 8×8 trên bus 32/64/128-bit. Nhóm này dùng identity reconstruction để kiểm tra luồng prediction/feedback.

[Full codec test](../tests/Coding_Reconstruction_Test/FULL_PIPELINE_CODEC_REPORT.md) nối prediction với TQ/ITQ và reconstruction. Test adapter xử lý ảnh 32×32 YUV420, một frame I4×4 và ba frame P; chroma U/V dùng DC mode. Luma đi qua `TqTop` TLM, chroma DC được xử lý bởi arithmetic classes trong adapter.

```sh
cmake -S components/h264 -B build/h264-vp \
  -DCMAKE_BUILD_TYPE=Debug -DH264_BUILD_TESTS=ON \
  -DSYSTEMC_INCLUDE_DIR=/opt/systemc-2.3.4/include \
  -DSYSTEMC_LIBRARY=/opt/systemc-2.3.4/lib/libsystemc.so
cmake --build build/h264-vp -j 4
ctest --test-dir build/h264-vp -R '^h264_prediction_cmb_intra_' --output-on-failure
ctest --test-dir build/h264-vp -L prediction --output-on-failure
ctest --test-dir build/h264-vp -R '^full_codec_' --output-on-failure
```

Label `prediction` gồm **15 test tích hợp** cho DMA, Intra, Inter và control. Component H.264 import các library prediction với suite riêng bị tắt, nên cần build standalone ở trên để chạy đủ test của Intra. Nhóm **36 test `full_codec_*`** chỉ được đăng ký khi CMake tìm thấy Python3 và FFmpeg; kiểm tra danh sách bằng `ctest --test-dir build/h264-vp -N -R '^full_codec_'`.

## Phạm vi sử dụng

Module hỗ trợ progressive 8-bit YUV 4:2:0 với luma 4×4/16×16 và chroma 8×8. Bên gọi chọn partition, tổ chức macroblock, cung cấp slice/constrained-intra availability và ghép TQ/ITQ, entropy, deblocking. `ImportReconstructed` yêu cầu đúng picture và thứ tự context; mode RAM lưu mode đã chọn, chưa xuất syntax MPM.

Automatic partition RDO, luma 8×8 High Profile, bit-depth trên 8 và MBAFF chưa được triển khai. Timing VP chưa được đối chiếu cycle với RTL. Pipeline codec hiện nằm trong test adapter; Intra chưa được đưa vào install/export của package H.264.
