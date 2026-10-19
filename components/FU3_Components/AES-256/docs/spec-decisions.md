# HDS và quy ước mô hình

Nguồn: `FTEL_CDC_IP_AES_Specification (1).pdf`, 23 trang. Số trang dưới đây
là số trang vật lý PDF. SHA-256 của tài liệu:
`b339ebbd0fdff57c6f483d062920f05b3a861e505df52b697de171c2c7229b45`.

Lõi thực hiện AES-128/256 cho block 128 bit theo FIPS 197 được HDS viện dẫn
ở trang 8. KEY0/BLOCK0 là word cao nhất; AES-128 dùng KEY0..3. IP không có
AES-192, IV, chaining, DMA handshake hoặc chứng nhận FIPS.

HDS còn các điểm chưa rõ hoặc mâu thuẫn. Bảng dưới ghi lựa chọn hiện tại của
VP để phục vụ tích hợp; các lựa chọn này chưa thay thế yêu cầu HDS chính thức.

| ID | Nội dung cần xác nhận và nguồn | Quy ước hiện tại |
| --- | --- | --- |
| HDS-Q01 | Cực tính và kiểu reset không thống nhất; p.11 §6.2, p.18 AES-R01/R05 | Reset đồng bộ active-low cho bus/core |
| HDS-Q02 | Dịch local CSR index thành byte address, alignment và alias; pp.10,14-18, AES-R03 | Byte offset = index × 4; không alias |
| HDS-Q03 | Baseline result decode và lát bit RESULT0..3; pp.15-18, AES-R02 | Mặc định đọc đủ 4 word MSW-first; tùy chọn mux lỗi theo mô tả RTL |
| HDS-Q04 | Response/completion cho burst/size không hỗ trợ; p.18 AES-R04 | Trả lỗi TLM, không tái hiện stall RTL |
| HDS-Q05 | Giới hạn tham số bus và width adaptation; p.10, p.18 AES-R06 | Target cố định 32 bit |
| HDS-Q06 | Latency/throughput và điểm đo; pp.11-12 không bảo đảm fixed latency | INIT 16, NEXT 64 chu kỳ mặc định; cấu hình được |
| HDS-Q07 | Reset values, READY sau reset, retention và hủy operation; pp.11,14-16 | Control/data/result = 0; READY=1, VALID=0; xóa tác vụ |
| HDS-Q08 | Command/ghi KEY/BLOCK/CONFIG khi busy; pp.12,16-17 | Từ chối ghi dữ liệu/cấu hình và command khác 0 |
| HDS-Q09 | INIT và NEXT cùng bằng 1; p.16 | INIT ưu tiên |
| HDS-Q10 | NEXT trước khi INIT hoàn tất hoặc chưa khởi tạo khóa; p.17 | Từ chối, không thay đổi trạng thái |
| HDS-Q11 | Thời điểm set/clear cờ, read-to-clear và kết quả cũ khi busy; pp.12,16 | INIT/NEXT xóa VALID; đọc giữ cờ; cập nhật toàn block lúc hoàn tất |
| HDS-Q12 | Thay khóa/KEYLEN/ENCDEC và tái sử dụng schedule; pp.12,16-17 | Ghi khóa/đổi KEYLEN cần INIT lại; đổi hướng giữ schedule |
| HDS-Q13 | Thời điểm chốt key/input/config; pp.12,17 | INIT chốt khóa; NEXT chốt input/hướng |
| HDS-Q14 | FIFO/interrupt/APB/UART trong phần giới thiệu không khớp phần interface/exclusion; pp.7,9,12-14 | TLM endpoint hướng AXI; không IRQ/FIFO/APB/UART |
| HDS-Q15 | Cover V1.0, history V2.0 và heading/reference không khớp | Dùng đúng bản PDF theo hash ở trên |
| HDS-Q16 | SoC base/aperture, offsets ngoài map và ghi RO; pp.14-17 | FU3 base 0x40000000, bus window 64 KiB; model 0x400 byte; RO/reserved writes bỏ qua |

Khi HDS được cập nhật, cần ghi nhận revision/section mới, sửa expected liên
quan trong test và cập nhật `tests/hds-requirements.json`. Không dùng kết quả
model hoặc OpenSSL để quyết định thay cho yêu cầu còn mở.

## Hợp đồng TLM

- `b_transport` được gọi từ `SC_THREAD`, nhận một word 32 bit aligned.
- Byte-enable merge vào dữ liệu lưu trữ; lane đọc bị disable giữ nguyên byte
  của caller. FU3 fabric chia burst và mở rộng narrow access thành word lanes.
- Incoming delay được tiêu thụ; truy cập thanh ghi chờ cạnh clock và được
  tuần tự hóa. Không cấp DMI; debug chỉ cho phép đọc không side effect.
- Reset được lấy mẫu sẽ xóa trạng thái và hủy các caller đang chờ delay,
  lock hoặc clock. Request vào lúc reset thấp vẫn bị hủy sau khi reset nhả.
- Các lỗi payload/trình tự trả TLM error theo [registers.md](registers.md).

Đây là hợp đồng VP ở mức transaction. Nó không mô phỏng handshake/ID của
từng kênh AXI, CDC hoặc waveform bus. `ObservedRtlMux` chỉ đổi cách đọc result:
RESULT1/2 bằng 0, RESULT0/3 trả word thấp. RESULT0 là suy luận từ phép gán
128 bit sang đường đọc 32 bit, chưa phải mapping đã xác nhận.

Nguồn thuật toán: [FIPS 197-upd1](https://doi.org/10.6028/NIST.FIPS.197-upd1).
Nguồn expected cố định: [FIPS 197 Appendix C.1/C.3](https://nvlpubs.nist.gov/nistpubs/fips/nist.fips.197.pdf).
Hướng dẫn chạy kiểm thử: [testing.md](testing.md).
