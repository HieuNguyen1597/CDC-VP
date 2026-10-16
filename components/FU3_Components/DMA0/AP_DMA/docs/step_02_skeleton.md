# Bước 2 — Khung module và testbench

> Tài liệu milestone trước. Engine M2M hiện đã triển khai; xem [bước 4](step_04_m2m.md). Người dùng đã xác nhận unit suy ra từ alignment và tham số, không cần chọn unit cố định.


## Các file và vai trò

- `include/fu3/dma/ap_dma.h`: khai báo module ApDma và giao tiếp theo bước 1.
- `src/ap_dma.cpp`: đăng ký callback b_transport; giữ IRQ/clear inactive.
- `tests/test_skeleton.cpp`: CPU initiator giả và memory target giả, bind đủ port.
- `CMakeLists.txt`: build library và CTest, yêu cầu SystemC 2.3.4/C++17.

Target socket gọi b_transport khi CPU phát transaction. Trong bước khung,
callback trả COMMAND_ERROR và không thay buffer; chưa có thanh ghi nào được
triển khai. Đây là placeholder rõ ràng, không phải hành vi register map cuối cùng.
Master socket đã bind nhưng chưa phát transaction. Test kiểm tra đúng giới hạn này.

SC_METHOD ghi các output inactive lúc khởi tạo và khi reset đổi. Chưa triển khai
reset state hay quyết định reset đồng bộ/bất đồng bộ của phần cứng.

## Build

Cần bản SystemC 2.3.4 được build tương thích C++17/compiler ABI của model.
Thay `/home/vinhnt/Documents/deps/systemc-2.3.4` bằng prefix cài đặt thực tế:

```bash
cmake -S components/FU3_Components/DMA0/AP_DMA \
  -B /tmp/fu3-ap-dma-step02 \
  -DSYSTEMC_HOME=/home/vinhnt/Documents/deps/systemc-2.3.4 \
  -DBUILD_TESTING=ON
cmake --build /tmp/fu3-ap-dma-step02 --parallel 2
ctest --test-dir /tmp/fu3-ap-dma-step02 --output-on-failure
```

Có thể truyền trực tiếp SYSTEMC_INCLUDE_DIR và SYSTEMC_LIBRARY nếu prefix có
layout khác. CMake kiểm macro phiên bản header; test kiểm version runtime.

## Kết quả kiểm tra hiện tại

Đã cài SystemC 2.3.4 từ tag `2.3.4` của repository chính thức
`https://github.com/accellera-official/systemc`, commit
`5fc1469339775b54b1fcc2020ac744a58be5b50e`.
Prefix: `/home/vinhnt/Documents/deps/systemc-2.3.4`.
Build Release shared library bằng GCC 13.3.0, C++17.
Configure và build khung DMA thành công; CTest: **1/1 PASS**.
Chỉ xác nhận khung giao tiếp, chưa kiểm thanh ghi hoặc truyền dữ liệu.
Compiler có cảnh báo overloaded virtual trong header TLM của SystemC 2.3.4.
Chưa thay đổi build root của CDC-VP; component đang cấu hình standalone.

## Tiếp theo

Sau khi build/test khung thành công, triển khai register bank/reset/masks và decode.
Engine M2M ở bước sau nữa; unit được suy ra theo quy tắc đã ghi trong bước 4.
