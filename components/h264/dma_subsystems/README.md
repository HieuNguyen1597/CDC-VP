# H.264 DMA Subsystem

Author: VinhNT124

Mô hình DMA cho H.264, gồm core C++17 và wrapper SystemC 2.3.4/TLM-2.0. Module vận chuyển dữ liệu giữa CMB, search window, reconstructed frame và NAL buffer thông qua arbiter và AXI memory bridge. Wrapper cung cấp MMIO, shared DDR, IRQ và reset để tích hợp với platform CDC-VP.

## Chức năng

| Thành phần | Vai trò |
|---|---|
| CMB | Đọc current macroblock từ source frame, xuất Y16×16/U8×8/V8×8 |
| SW | Nạp reference search window cho List0/List1 và tái sử dụng vùng overlap |
| DF/REFM | Ghi reconstructed hoặc filtered pixels vào reference frame memory |
| NAL | Ghi output words, quản lý buffer capacity và stream length |
| Arbiter | Điều phối request của các DMA clients và giữ grant đến khi transaction hoàn tất |
| AXI bridge | Lập kế hoạch burst, xử lý byte lanes/strobes và response trên bus 32/64/128-bit |

## Cấu trúc và CMake targets

| Target | Vai trò |
|---|---|
| `h264_dma` | Core header-only, không phụ thuộc SystemC |
| `h264_dma_tlm` | Wrapper SystemC/TLM và MMIO |

Các alias tương ứng là `cdc::components::h264_dma` và `cdc::components::h264_dma_tlm`. Core nằm trong `common/`, `dma/` và `mem/`; wrapper trong `tlm/`; test riêng trong `tb/` và `tlm/tests/`. Component H.264 đăng ký và install/export các DMA targets này.

## Giao dịch và bộ nhớ

`DmaRequest.beats` là số internal transfers, mỗi transfer có `size` byte; tổng payload bằng `beats * size`, độc lập với AXI data width. Arbiter lưu bản sao request và write payload. Read completion trả các byte được yêu cầu trong `DmaResponse.data`.

Bridge căn địa chỉ theo bus beat, chọn lane/WSTRB và chia burst tại ranh giới 4 KiB hoặc burst limit. Mỗi write segment nhận một B response; chỉ byte có strobe được ghi. Segment đã thành công giữ nguyên khi segment sau gặp lỗi.

`service_one()` trả client đang giữ grant, kể cả khi transaction còn pending; `NONE` biểu thị idle. `run(max_steps)` tiếp tục xử lý qua các stall và báo lỗi nếu hết giới hạn khi vẫn còn work. `error()` giữ trạng thái lỗi đến khi subsystem idle và host gọi `clear_error()`. Delay trong `AxiBridgeConfig` tính theo service steps; read FIFO chứa tối đa 16 internal words, mỗi entry tối đa 4 byte.

Coded width/height phải dương, chia hết cho 16 và tối đa 1920×1088. Với planar YUV420, một frame cần `F = 3 * width * height / 2` byte; REFM gồm ba slots nên cần `3F` byte. Layout được kiểm tra về physical window, alignment theo bus width, capacity và overlap. `nal_capacity=0` suy ra capacity đến vùng kế tiếp hoặc cuối modeled DDR.

`DmaSubsystem` có thể dùng DDR riêng hoặc nhận `MemoryIf&`, `memory_base` và `memory_bytes` từ platform. Platform giữ ownership của external memory. `memory()` trả interface chung; `ddr()` chỉ dùng khi subsystem sở hữu private DDR.

## Client API

### CMB: current macroblock

`update_enable(true)` bắt đầu frame ở cạnh enable thấp→cao; giữ enable cao không tạo thêm start. `fetch_macroblock()` nhận tối đa một tile pending. `pixels()` sẵn sàng sau khi tất cả read thành công và trả planar Y256/U64/V64 samples.

Ở mode `WorkingSet`, gọi `select_working_frame(slot)` trước enable. Controller cung cấp source slot theo coding order; `RegisterBases.cmb_frames` và source capacity xác định số frame được cấp phát.

### SW: reference search window

`set_ref_slot(list, slot, picture_tag)` chọn và retag reference riêng cho từng list. `fill_window()` chỉ ready sau khi nạp xong toàn vùng Y/U/V; vùng overlap đã đọc thành công được tái sử dụng khi chuyển macroblock hoặc hàng.

Window bắt đầu tại tọa độ macroblock và được clip vào coded frame; `SwPlane` cung cấp origin/extent thực tế. Bên sử dụng xử lý search origin và padding biên. `invalidate()` xóa cả residency và reference selection, nên cần chọn slot lại. Chỉ sử dụng reference sau khi producer DF xác nhận frame đã hoàn tất.

### DF: reconstructed frame output

Producer cung cấp `MacroblockPixels` đã tái tạo. Ví dụ ghi một macroblock ở chế độ bypass, với `vp` là `DmaSubsystem` và `reconstructed` là pixel đầu vào:

```cpp
vp.df().latch_df_enable(false);
vp.df().set_ref_slot(1);
vp.df().on_sofm();
vp.df().schedule_macroblock(mb_x, mb_y, reconstructed, false);
vp.df().dma_fmdone();  // Gọi sau khi đã gửi toàn bộ work của frame.
vp.df().consume_done();
vp.run();
// Reference sẵn sàng khi vp.df().frame_complete() trả true.
```

Nếu bật filter, truyền tile đã lọc với `already_filtered=true`, hoặc đăng ký `set_filter(callback)` cho tile chưa lọc. Callback chạy vertical trước horizontal, hoàn tất rồi DMA mới ghi Y/U/V. Thiếu pixel input hoặc filter producer cần thiết sẽ bị từ chối.

`dma_fmdone(frame_id)` và `frame_complete(frame_id)` theo dõi completion theo frame, tránh done đến trễ làm hoàn tất nhầm frame mới. API cũ `latch_dis_idc(0/1)` tương ứng DF enable, khác với trường syntax `disable_deblocking_filter_idc`.

### NAL: output buffer

Mỗi flush lưu snapshot của các words đã nhận; nhiều flush append trong cùng activation. Pointer/count chỉ cập nhật theo response thành công, và capacity được kiểm tra trước enqueue.

`on_disable()` yêu cầu không còn output pending, reload pointer và giữ `STM_LEN` để host đọc. Activation tiếp theo đi qua disable rồi `begin_activation()`. `WordByteOrder` cấu hình thứ tự byte, mặc định little-endian. `accept_eos()` ghi bytes `00 00 01 0B`.

## Kết nối SystemC/TLM

Wrapper cung cấp các cổng:

| Cổng | Vai trò |
|---|---|
| `target_socket` | MMIO target, nhận offset local sau address decode |
| `master_socket` | DMA initiator, phát địa chỉ vật lý |
| `reset_n` | Reset active-low |
| `irq` | Level interrupt, nối với interrupt controller |

Map MMIO qua `bus_router.add_target()`, nối host và DMA master vào cùng router, và chia sẻ một DDR mapping. `H264DmaOptions.memory_base/memory_bytes` phải khớp physical RAM window.

[Platform test](tlm/tests/h264_dma_platform.cpp) dùng cấu hình minh họa:

| Vùng | Địa chỉ / kích thước |
|---|---|
| MMIO | `0x10080000`, 4 KiB |
| Shared DDR | `0x80000000`, 1 MiB |
| CMB | `0x80001000` |
| REFM | `0x80010000` |
| NAL | `0x80030000`, capacity 4 KiB |

Địa chỉ trên dành cho test; platform tích hợp tự cấu hình memory map. Host nạp source và đọc REFM/NAL qua `b_transport` trên shared bus.

## Thanh ghi MMIO

| Offset | Thanh ghi | Nội dung |
|---|---|---|
| `0x00` | SCON | ECE bit 0, GIE bit 1; cạnh ECE 0→1 chốt config và start |
| `0x04` | STAT | NORMAL bit 18, ERROR bit 17, BUSY bit 16, FMCNT bits 15:0 |
| `0x08` | FMSIZE | Coded width bits 26:16, height bits 10:0 |
| `0x0C` | DFCON | Enable bit 0; signed alpha/beta offsets cho producer |
| `0x10` | SPARA0 | QP, CMB mode và syntax/GOP fields; reset `0x12C` |
| `0x14` | SPARA1 | Working-set allocation bits 31:25, syntax widths, FMENC bits 15:0 |
| `0x18` | SPARA2 | Crop/delta fields |
| `0x1C` | REFM | Physical base của ba reference slots |
| `0x20` | NAL | Physical output base |
| `0x24` | CMB | Physical source base |
| `0x28` | STM_LEN | Số 32-bit words đã ghi thành công |

Config chỉ được ghi khi ECE=0 và BUSY=0. Start mới reset stream count/pointer; disable sau completion giữ `STM_LEN`. GIE điều khiển IRQ. Đọc STAT hoặc STM_LEN acknowledge IRQ và giữ các status/count fields; `transport_dbg` chỉ đọc và không acknowledge.

MMIO dùng giao dịch 32 bit căn chỉnh, register bytes little-endian và byte enables `00/FF`. Sai size/alignment trả BURST_ERROR; ghi read-only trả COMMAND_ERROR; sai reserved bits hoặc config phase trả GENERIC_ERROR. Offset chưa định nghĩa mặc định trả ADDRESS_ERROR; option `ReadZero` cho phép đọc zero.

QP hợp lệ trong khoảng 0..51; FMENC và working-set allocation phải khác 0. Crop phải giữ lại vùng ảnh hợp lệ. Config không hợp lệ kết thúc activation bằng ERROR.

## Workload, completion và reset

Cài `set_workload_provider(callback)` trước activation hoặc enqueue `H264ActivationWorkload` khi ECE=0/BUSY=0. Queued workloads có ưu tiên; mỗi start tiêu thụ một workload, kể cả khi config sai. Reset xóa queue và giữ callback/filter đã cài.

Workload cung cấp đầy đủ macroblocks của từng coded picture, mỗi tọa độ đúng một lần, source/reference slots, List0/List1 selections, picture tags, reconstructed/filtered pixels và NAL words. Số frame không vượt FMENC. CMB mode 1 xử lý một picture mỗi activation; mode 0 chọn source slot trong working set. FMCNT đếm số pictures hoàn tất trong activation; codec producer quyết định coding order và reference validity.

`H264MacroblockJob.reconstructed` chứa Y256/U64/V64 samples. DF bypass vẫn cần reconstructed pixels; `replay_source_for_bypass=true` chỉ dành cho DMA-only tests khi DF tắt. SW cache được invalidate giữa các pictures; B-picture yêu cầu cả hai reference lists đã resident. NAL words append qua các frames, với EOS tùy chọn.

NORMAL/IRQ chỉ xuất hiện sau khi REFM và NAL responses đều thành công. Downstream non-OK response hoặc lỗi core đặt ERROR. `max_service_steps` giới hạn core bị stall; target/harness cần watchdog riêng nếu `b_transport` không trả về. Register latency mặc định là 10 ns, service latency 8 ns.

Hạ ECE khi BUSY hoàn tất transaction đang chạy, dừng queued work rồi đặt ERROR. Reset active-low xóa registers/status/IRQ/count và vô hiệu hóa completion cũ. Các bytes đã được downstream nhận có thể đã tác động DDR; completion cũ sau reset không phát IRQ cho activation mới.

## Build và test riêng

Yêu cầu CMake từ 3.16 và compiler C++17. Các lệnh dưới đây chạy từ thư mục gốc `CDC-VP` trong WSL/Linux.

### Core không dùng SystemC

```sh
cmake -S components/h264/dma_subsystems -B build/h264-dma-core \
  -DCMAKE_BUILD_TYPE=Debug -DH264_BUILD_TLM=OFF -DBUILD_TESTING=ON
cmake --build build/h264-dma-core -j 4
ctest --test-dir build/h264-dma-core --output-on-failure
```

### Core và SystemC/TLM

SystemC 2.3.4 phải tương thích compiler và C++ ABI với project. Thay prefix dưới đây bằng vị trí cài đặt trên máy:

```sh
cmake -S components/h264/dma_subsystems -B build/h264-dma-tlm \
  -DCMAKE_BUILD_TYPE=Debug -DH264_BUILD_TLM=ON -DBUILD_TESTING=ON \
  -DSYSTEMC_HOME=/opt/systemc-2.3.4
cmake --build build/h264-dma-tlm -j 4
ctest --test-dir build/h264-dma-tlm --output-on-failure
```

| Test | Phạm vi |
|---|---|
| `h264_dma_contracts` | CMB/SW/DF/NAL, arbiter, AXI bridge, capacity, lỗi và memory lifetime |
| `h264_dma_shared_bus` | MMIO, DDR, IRQ và reset trên bus 32-bit |
| `h264_dma_shared_bus_64` | Cùng contract trên bus 64-bit |
| `h264_dma_shared_bus_128` | Cùng contract trên bus 128-bit |
| `h264_dma_response_timeout` | Service-step limit khi channel bị stall |

Build core-only đăng ký **1 test** với **272 assertions**; build có TLM đăng ký **5 test**. Kết quả kiểm chứng, sanitizer và logs được lưu trong [VALIDATION.md](VALIDATION.md).

## Test tích hợp H.264

[Prediction_Test](../tests/Prediction_Test/README.md) gồm 15 test kết nối CMB/SW/DF/NAL với Intra/Inter và control/reset trên bus 32/64/128-bit. Nhóm này dùng `H264Arb` và [DmaTransport](../top/include/h264/top/dma_transport.h) với core arbiter/AXI bridge. Test MMIO wrapper chạy trong suite DMA riêng ở trên.

[Full codec test](../tests/Coding_Reconstruction_Test/FULL_PIPELINE_CODEC_REPORT.md) nối DMA/prediction với TQ/ITQ, CAVLC/NAL và reconstruction. Test adapter xử lý ảnh 32×32 YUV420, bốn frame I-P-P-P, một List0 reference và P16×16 với zero MV.

```sh
cmake -S components/h264 -B build/h264-vp \
  -DCMAKE_BUILD_TYPE=Debug -DH264_BUILD_TESTS=ON \
  -DSYSTEMC_INCLUDE_DIR=/opt/systemc-2.3.4/include \
  -DSYSTEMC_LIBRARY=/opt/systemc-2.3.4/lib/libsystemc.so
cmake --build build/h264-vp -j 4
ctest --test-dir build/h264-vp \
  -R '^h264_dma_(contracts|shared_bus|response_timeout)' --output-on-failure
ctest --test-dir build/h264-vp -L prediction --output-on-failure
ctest --test-dir build/h264-vp -R '^full_codec_' --output-on-failure
```

Nhóm **36 test `full_codec_*`** chỉ được đăng ký khi CMake tìm thấy Python3 và FFmpeg; kiểm tra danh sách bằng `ctest --test-dir build/h264-vp -N -R '^full_codec_'`.

## Phạm vi sử dụng

Module phục vụ functional DMA và driver integration với shared DDR, MMIO, IRQ/reset. Codec producer đảm nhiệm scheduling, prediction, transform, entropy/NAL generation và deblocking arithmetic. Timing ở mức service steps/TLM chưa được đối chiếu cycle/pin với RTL; platform test sử dụng SystemC host driver.

Chi tiết layout, cấu hình và client contracts nằm trong [DMA_SUBSYSTEM_SPEC.md](DMA_SUBSYSTEM_SPEC.md).
