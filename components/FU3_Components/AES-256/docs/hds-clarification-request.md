# Tổng hợp vấn đề cần người phụ trách HDS xác nhận

**Trạng thái: DRAFT / CHƯA GỬI RA NGOÀI.** Theo yêu cầu của người dùng,
chỉ tổng hợp để người dùng chuyển tiếp. Chưa có phản hồi hoặc phê duyệt
của người chịu trách nhiệm HDS; mọi mục dưới đây vẫn OPEN.

Đối tượng: `FTEL_CDC_IP_AES_Specification (1).pdf`, 23 trang.
SHA-256: `b339ebbd0fdff57c6f483d062920f05b3a861e505df52b697de171c2c7229b45`.
Các số trang là số trang vật lý PDF. Cover V1.0, lịch sử ghi V2.0 ngày
03/04/2026; chưa tự coi một revision khác là baseline đã duyệt.

**Mục đích:** chốt expected behavior trước khi dùng kết quả VP để kết luận
`SPEC_PASS/FAIL`. Các giá trị VP hiện tại dưới đây chỉ là thông tin triển khai,
không phải lựa chọn đã được chủ HDS đồng ý.

| ID / phân loại | Nguồn HDS và vấn đề | Câu hỏi cần xác nhận / nội dung cần bổ sung | VP hiện tại, chỉ để tham khảo |
| --- | --- | --- | --- |
| HDS-Q01 — SPEC_CONFLICT | p.11 §6.2; p.18 AES-R01/R05: wrapper reset active-low nối trực tiếp vào core active-high; comment async không khớp sequential logic | Chốt cực tính tại cổng IP và core, vị trí đảo reset, synchronous/asynchronous assertion/release; cung cấp waveform reset hợp lệ | Đồng bộ active-low cho toàn VP |
| HDS-Q02 — SPEC_UNDEFINED | p.10, pp.14-17, AES-R03: local CSR index và low 8 address bits không định nghĩa được byte map nhất quán | Cung cấp bảng byte offset phần mềm; quy tắc shift, alignment, decode bit địa chỉ và alias | index × 4, không alias |
| HDS-Q03 — SPEC_CONFLICT | pp.15-17, AES-R02: mô tả decode hiện tại thiếu RESULT1/2, trong khi thao tác yêu cầu sửa để đọc đầy đủ | Baseline nghiệm thu là RTL đang giao hay sau sửa? Chốt lát bit của RESULT0..3, đặc biệt RESULT0; cung cấp bảng expected readback | Mặc định trả đủ 4 word; option mux lỗi trả low word ở RESULT0/3 là suy luận, chưa duyệt |
| HDS-Q04 — SPEC_UNDEFINED | p.9, p.18 AES-R04: request burst/size không hỗ trợ có thể không có response | Chốt B/R response code, việc chấp nhận request, số beat trả, thời hạn hoàn tất và ảnh hưởng CSR; có cần tái hiện stall hiện tại không? | TLM error hữu hạn |
| HDS-Q05 — SPEC_UNDEFINED | p.10 table 5-1; AES-R06: bus có tham số nhưng CSR cố định 32 bit | Liệt kê bộ ADDR/DATA/STRB/ID_WIDTH hợp lệ; giới hạn cấu hình; cần width adapter hay chỉ cho phép mặc định? | Target 32 bit; không mô hình raw AXI ID |
| HDS-Q06 — SPEC_UNDEFINED | p.11 §6.3 nêu không có bảo đảm fixed latency/throughput; p.12 yêu cầu polling | Xác nhận timing không nằm trong tiêu chí conformance, hay bổ sung latency/throughput/bounds và điểm đo cho INIT/encrypt/decrypt; tách 128/256 nếu khác | INIT 16, NEXT 64 cycles, cấu hình được |
| HDS-Q07 — SPEC_UNDEFINED | p.11; pp.14-16 không có bảng reset values đầy đủ | Bổ sung reset value/mask cho từng register/hidden state, READY sau reset, retention, hủy operation và xóa khóa | Giá trị dữ liệu/control bằng 0, READY=1, VALID=0 |
| HDS-Q08 — SPEC_UNDEFINED | pp.12,16-17 chỉ mô tả normal sequence | Với INIT/NEXT/KEY/BLOCK/CONFIG khi busy: ignore, reject, queue hay update? Có side effect/error status nào? | Reject ghi cấu hình/dữ liệu và command khác 0 khi busy |
| HDS-Q09 — SPEC_UNDEFINED | p.16 CTRL mô tả từng bit, không mô tả cả hai cùng 1 | CTRL.INIT và CTRL.NEXT cùng 1: ưu tiên nào, thực hiện cả hai hay lỗi? Readback pulse thế nào? | INIT ưu tiên |
| HDS-Q10 — SPEC_UNDEFINED | p.17 §9.1 yêu cầu INIT trước NEXT, không nêu vi phạm | NEXT trước INIT, trong INIT hoặc sau thay khóa chưa INIT lại: expected response/state/result là gì? | Từ chối NEXT nếu lịch khóa chưa sẵn sàng |
| HDS-Q11 — SPEC_UNDEFINED | p.12 §6.6; p.16 STATUS mới định nghĩa ý nghĩa cờ | Chốt event/edge set-clear READY/VALID; sticky hay pulse; read-to-clear; old result khi busy; cập nhật từng word hay toàn block | INIT/NEXT xóa VALID, read giữ; cập nhật kết quả toàn block khi xong |
| HDS-Q12 — SPEC_UNDEFINED | pp.12,16-17 chưa nêu schedule invalidation/reuse | Ghi KEY, đổi KEYLEN, đổi ENCDEC có bắt buộc INIT lại không? Ghi KEY4..7 trong AES-128 ảnh hưởng gì? | Mọi key write/key-length change invalidates; đổi direction giữ schedule |
| HDS-Q13 — SPEC_UNDEFINED | pp.12,17 không nêu chính xác lúc sampling | Key/input/config được chốt ở CSR write, command pulse hay các chu kỳ xử lý? Cho phép ghi block tiếp theo trong khi chạy không? | INIT snapshot key; NEXT snapshot block/direction |
| HDS-Q14 — SPEC_CONFLICT | p.7 nói FIFO/pipeline/interrupt, p.9 ghi không có interrupt output; heading UART và mô tả APB không khớp phần AXI chi tiết | Xác nhận danh sách interface/features chính thức; bỏ/sửa template prose. IRQ, FIFO, APB/UART có thuộc IP này không? | Không IRQ/DMA handshake/FIFO/UART/APB |
| HDS-Q15 — SPEC_CONFLICT | Cover V1.0 so với p.4 V2.0; header FU2/V5.7; reference §8.1/table 8-2 không khớp numbering | Cung cấp revision/baseline chính thức, ngày phê duyệt, corrected section references và phạm vi áp dụng FU3 | Pin theo SHA-256 PDF nhận được |
| HDS-Q16 — SPEC_UNDEFINED | pp.14-17 chỉ có local CSR map đến index 0x33 | Chốt base/aperture tại SoC hoặc trỏ integration spec; expected cho offsets ngoài map, RO writes và địa chỉ alias/không hợp lệ | FU3 bus base 0x40000000, cửa sổ 64KiB; model 0x400 bytes, RO/reserved writes bỏ qua |

Ưu tiên đề xuất: Q01/Q02/Q03/Q15 trước khi nghiệm thu luồng firmware qua CSR;
Q04/Q05/Q14/Q16 cho tích hợp bus; các mục còn lại cho state/timing/corner cases.
Đây là thứ tự triển khai đề xuất, không phải mức độ ưu tiên đã được HDS phê duyệt.

Đề nghị người phụ trách trả lời từng ID theo mẫu:

| Trường | Nội dung cần điền |
| --- | --- |
| Question ID | HDS-Qxx |
| Owner / người duyệt | Tên người chịu trách nhiệm |
| Expected behavior được duyệt | Bảng field, điều kiện, event/edge và kết quả; không chỉ ghi “đồng ý” chung |
| HDS revision / section / change reference | Revision được cập nhật và vị trí yêu cầu |
| Baseline RTL áp dụng | Revision/commit hoặc bản giao tương ứng |
| Ngày xác nhận | Ngày phê duyệt |
| Tác động test | Test cần bổ sung/sửa; expected source chính thức |

Chỉ sau khi có xác nhận mới chuyển trạng thái yêu cầu và thay expected tương ứng.
Không dùng policy hiện tại hoặc kết quả OpenSSL làm câu trả lời thay người phụ trách.
Ma trận đầy đủ: [hds-verification-matrix.md](hds-verification-matrix.md).
