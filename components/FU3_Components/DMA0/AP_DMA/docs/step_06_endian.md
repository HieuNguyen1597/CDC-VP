# Bước 6 — Endian swap

## Thanh ghi và hành vi

HDS §7, CH_MODE_CONFIG, trang PDF 42 quy định END_SWAP ở bit [29:28]:

| Mode | Giá trị CH_MODE_CONFIG | Input bytes | Output bytes |
|---|---|---|---|
| 00 | 0x00000000 | 10 21 32 43 | 10 21 32 43 |
| 01 | 0x10000000 | 10 21 32 43 | 21 10 43 32 |
| 10 | 0x20000000 | 10 21 32 43 | 43 32 21 10 |
| 11 | 0x30000000 | Reserved | Start trả COMMAND_ERROR |

Chỉ thay thứ tự byte dữ liệu payload. Không đổi byte order giao tiếp thanh ghi,
địa chỉ nguồn/đích, ID peripheral hay chiều increment.

## Cách triển khai

Swap một lần tại đầu vào FIFO sau khi read service thành công. Mỗi kênh có
swap_pending tối đa một nhóm 4 byte và swap_count. Byte vào được tích lũy; khi
đủ nhóm 1/2/4 byte, đảo thứ tự nhóm và đưa vào phần FIFO có thể ghi đi.
Byte chưa đủ nhóm chưa được cấp cho write engine.

Ví dụ mode32 và read burst max=1:

```text
Read 10 -> pending [10]          -> chưa write
Read 21 -> pending [10 21]       -> chưa write
Read 32 -> pending [10 21 32]    -> chưa write
Read 43 -> FIFO [43 32 21 10]    -> write được
```

Vì giữ nhóm giữa các burst, đổi burst max hay unit suy ra từ alignment không
làm đổi kết quả stream. Write có thể tách thành 1/2/4 byte mà không swap lại.
Mỗi kênh giữ nhóm riêng; start/reset xóa nhóm cũ. Các byte pending được tính
trong ngân sách FIFO 32 byte, không thêm capacity ngoài cấu hình phần cứng.
CH_FIFO_STATUS chỉ tính complete groups vào write-side unassigned data.
Đây là cách biểu diễn functional nội bộ, chưa claim layout FIFO vật lý RTL.

M2P/P2M dùng cùng pipeline. P2M nhận từng byte/request vẫn phát RX clear sau
mỗi service; không chờ đủ group mới acknowledge peripheral. Write memory chờ
đủ group. M2P phát byte đã swap theo TX request bình thường.

## Giới hạn và policy cần đối chiếu RTL

Spec chỉ ghi swap within 16/32 bits, chưa mô tả chi tiết grouping khi địa chỉ lệch
và partial group. Model chọn group bắt đầu từ byte đầu của command, giữ xuyên
burst/request. Đây là policy functional tường minh, cần đối chiếu RTL nếu muốn
khớp theo bus lane/word thay vì stream. Test xác nhận policy này, không chứng minh
mọi unaligned/endian corner case của RTL.

Mode16 yêu cầu byte count chia hết 2, mode32 chia hết 4. Start với partial group
bị từ chối trước mọi bus side effect; không tự pad, bỏ hoặc đảo một phần nhóm.
Đây là phạm vi milestone, không phải kết luận rằng phần cứng cấm partial group.
Mode00 vẫn xử lý số byte lẻ như trước. Mode11 reserved bị từ chối.

## Kiểm chứng

`test_fu3_ap_dma_endian` gồm:

- Vector biết trước cho mode00/01/10.
- 60 cấu hình: 3 mode × 4 alignment × 5 read burst max (1,3,5,31,127),
  write burst max=7, 260 byte xuyên biên 4 KiB và nhiều vòng FIFO.
- Guard bytes quanh destination, completion count, hai kênh chạy mode khác nhau.
- Reserved/partial group không phát master traffic.
- Reset giữa partial group và restart không mang byte cũ sang command mới.

`test_fu3_ap_dma_peripheral` bổ sung M2P swap16/P2M swap32 xuyên các credit 1 byte.
Toàn bộ regression **4/4 PASS**, SystemC 2.3.4.

Chạy riêng:

```bash
cmake --build /tmp/fu3-ap-dma-step02 --parallel 2
ctest --test-dir /tmp/fu3-ap-dma-step02 -R '^fu3_ap_dma_endian$' -V
```

Command-chain, wrapper DTYPE/FLUSH, weighted arbitration, read/write overlap,
multiple pending và timeout/watchdog vẫn chưa triển khai ở bước này.
