# Báo cáo kiểm thử AES VP/TLM

Ngày kiểm chứng: **2026-10-19**. Phạm vi: component AES-128/256 và kết nối
với FU3 bus, theo mô hình chức năng đã mô tả trong
[spec-decisions.md](spec-decisions.md). Chưa kiểm chứng tương đương RTL
hoặc boot firmware trên toàn bộ SoC.

## Kết quả sau khi tách SPEC / POLICY / REFERENCE

| Cấu hình | SPEC_PASS | POLICY_PASS | REFERENCE_PASS | Lỗi test |
| --- | --- | --- | --- | --- |
| Debug | 7 | 28 | 4 | 0 |
| UBSan | 7 | 28 | 4 | 0 |
| Release | 7 | 28 | 4 | 0 |

Cả ba build dùng `-Wall -Wextra -Wpedantic -Werror`. Môi trường: WSL,
GCC 15.2.0, SystemC 2.3.4, OpenSSL 3.5.5. Tổng cộng 117 lượt chạy thành công,
không có test bỏ qua trong ba suite cuối cùng. **Không được diễn giải con số
này thành 39 yêu cầu HDS đã đạt hoặc toàn IP đã đạt HDS.**

Ma trận đối chiếu có **38 mục**: 22 `SPEC_CLEAR`, 12 `SPEC_UNDEFINED`,
4 `SPEC_CONFLICT`. Trạng thái kiểm chứng của 22 mục rõ gồm:

- 7 `SPEC_PASS`: output lõi AES-128/256 hai chiều, lựa chọn nửa khóa AES-128,
  trạng thái từng bước và toàn bộ round key của các vector được chỉ ra trong ma trận.
- 9 `SPEC_BLOCKED`: yêu cầu rõ nhưng phụ thuộc reset/address/result đang mở.
- 6 `SPEC_NOT_TESTED`: chưa có test SPEC riêng, chỉ có code review, phạm vi
  loại trừ hoặc evidence chức năng gián tiếp; không tự nâng thành PASS.

Chi tiết từng chức năng/nguồn/expected/test/blocker nằm trong
[hds-verification-matrix.md](hds-verification-matrix.md).
16 câu hỏi tương ứng với các mục chưa rõ/mâu thuẫn đã được tổng hợp trong
[hds-clarification-request.md](hds-clarification-request.md), **chưa gửi ra ngoài**.

Expected cho bảy test SPEC là hằng số từ FIPS 197 Appendix C.1/C.3,
được HDS p.8 §1.2 viện dẫn. Chúng không lấy output từ model hoặc OpenSSL.
HDS không đính kèm known-answer vector riêng. Các test chỉ dùng API lõi,
không cần chọn policy địa chỉ/reset/result của CSR để tạo SPEC verdict.
Hai test mới đọc fixture gồm **248 trạng thái/key literal** từ bản FIPS có
SHA-256 `251dfe0b5dc283abaf364adf586f7ec6dc4e495335d48dd5ee0fee6c5961da8a`.
Đã so sánh thứ tự và giá trị từng phase, đủ 10/14 round, bỏ MixColumns ở round
cuối, đủ 11/15 round key và thứ tự đảo khi giải mã. Trace là observer chỉ đọc
trên phép transform thực tế, không phải thuật toán tham chiếu viết lại trong test.

Suite mặc định có 35 test; bật `FU3_AES_ORACLE_TESTS=ON` thành 39 test.
Các regression cũ còn trộn field HDS với policy VP đều được xếp POLICY.
Đối chiếu OpenSSL được xếp REFERENCE cho **1.158 bộ khóa/input**:

- AES-128: 515 bộ; AES-256: 643 bộ.
- Gồm mẫu cố định, từng bit khóa, từng bit input và dữ liệu pseudorandom có seed.
- Mỗi bộ kiểm tra cả mã hóa và giải mã, tại lõi và qua toàn bộ luồng MMIO.
- **5.272 phép so sánh kết quả độc lập mỗi suite**, gồm 640 phép kiểm tra thêm
  khi tái sử dụng schedule/đổi mode/rekey; cộng 515 phép kiểm tra
  AES-128 không sử dụng nửa khóa thấp.

Giải mã dùng kết quả OpenSSL độc lập cho input, không chỉ kiểm tra vòng
mã hóa rồi giải mã bằng chính model. Kết quả này không cấp SPEC_PASS/FAIL.

Năm kiểm tra Python riêng cho bộ báo cáo cũng đạt: policy/reference marker
không được dùng làm SPEC_PASS; crash/skip không thành SPEC_FAIL; mục undefined,
conflict hoặc blocked không được tự nâng thành PASS.

## Đối chiếu checklist 10 thành phần

Tên test trong bảng bỏ tiền tố `fu3_aes_policy_`; oracle dùng tiền tố
`fu3_aes_reference_`. Bảng này là inventory regression VP, **không phải bảng
SPEC_PASS**; trạng thái HDS chính thức của từng mục ở ma trận được liên kết trên.

| Thành phần | Nội dung đã kiểm tra | Test chính |
| --- | --- | --- |
| AXI/TLM Interface | Phản hồi MMIO; payload lỗi; DMI/debug; delay; byte enables; burst/narrow access qua bus | protocol, access_matrix, strobes, bus_lanes |
| Register Model | ID/reset; quyền RO/WO; reserved; mask; thứ tự byte/word; result mux | registers, strobes, result_mux |
| AES-128 Engine | Mã hóa/giải mã; vector cố định và OpenSSL; đọc đủ kết quả | aes128, core_oracle, mmio_oracle_128, bus_aes128 |
| AES-256 Engine | Mã hóa/giải mã; vector cố định và OpenSSL; dùng đủ khóa 256 bit | aes256, core_oracle, mmio_oracle_256, bus |
| Key Expansion | INIT; đổi khóa/độ dài; tái sử dụng lịch khóa khi đổi hướng; clear/uninitialized | commands, core, core_oracle |
| AES Controller | INIT/NEXT; INIT ưu tiên khi ghi cả hai; busy protection; nhiều block; giao dịch đồng thời | commands, status_flow, concurrency |
| Status Logic | READY/VALID; giữ kết quả cũ khi busy; cập nhật kết quả khi hoàn tất; đọc không xóa VALID | status_flow, timing, latency_* |
| Reset Model | Reset lúc INIT/NEXT/idle; hủy giao dịch chờ; xung không được lấy mẫu; phục hồi | reset, reset_edges, reset_entry, concurrency |
| Timing Model | Chu kỳ mặc định và 1/1, 2/3, 17/65, 257/513; nhiều chu kỳ clock; dừng clock | timing, latency_*, init_zero, block_zero |
| Testbench | Watchdog; test không phụ thuộc assert; reference độc lập; Debug/UBSan/Release; log/JUnit | scripts/run_full.sh và toàn bộ suite |

## Các nhánh bổ sung ở lần kiểm tra cuối

- `busy_matrix`: mọi KEY/BLOCK, bốn giá trị CONFIG, các tổ hợp command và
  byte-enable bằng 0 khi đang INIT/NEXT; kiểm tra output sau các ghi bị từ chối.
- `reset_boundaries`: cả AES-128/256, INIT/NEXT, reset trước/sau cạnh hoàn tất
  1 ps; kiểm tra không còn result/VALID/schedule cũ và phục hồi bình thường.
- `debug_invariants`: sweep toàn bộ địa chỉ debug, đảm bảo ghi không thay đổi
  trạng thái ẩn và đọc debug không làm tiến clock/job/command pulse.
- `hds_fields`: dùng literal field/index HDS theo address policy đã công bố,
  kiểm cả bốn CONFIG mode với INIT mới, kể cả INIT trực tiếp trong mode giải mã.
- `sequence_oracle`: nhiều block khác nhau dưới cùng schedule, đổi hướng,
  đổi key length, rekey và INIT lại; expected độc lập từ OpenSSL.

Các test mới đều đạt. Không phát hiện thêm sai lệch chức năng trong những
trường hợp này. Các test CSR trên vẫn là POLICY/REFERENCE do HDS chưa chốt
reset/address/result; không nâng 9 mục BLOCKED thành SPEC_PASS.

## Lỗi đã sửa trong lần audit trước

**Vi phạm policy VP: giao dịch bắt đầu khi reset active có thể được thực hiện sau annotated delay.**

Tái hiện: IP có kết quả hợp lệ; phát xung reset thấp dài 1 ns giữa hai cạnh
clock; gửi ghi CONFIG trong xung này với delay 40 ns. Xung không được core
lấy mẫu nên `reset_epoch_` không đổi. Bản cũ chỉ kiểm tra reset sau delay,
vì vậy có thể chấp nhận ghi sau khi reset đã nhả, trái hợp đồng VP về việc
từ chối giao dịch bắt đầu trong reset.

Đã sửa `AesController::transport` để ghi nhận trạng thái reset ngay khi nhận
request và giữ điều kiện hủy này qua delay. Regression `reset_entry` đã
**FAIL trước sửa** và hiện là **POLICY_PASS sau sửa** trong cả ba cấu hình.
Đây không phải SPEC_FAIL vì HDS không định nghĩa hợp đồng annotated delay.
Test cũng xác
nhận CONFIG không đổi và xung reset không được lấy mẫu không xóa trạng thái
core. Log trước sửa: `build/fu3-aes/reset-entry-before-fix.log`.

## Coverage và giới hạn

Coverage do GCC/gcov đo trên hai file triển khai qua các test Debug,
bao gồm cả POLICY/REFERENCE; **không phải coverage yêu cầu HDS**:

| File | Dòng thực thi đã chạy | Nhánh đã đi qua, gồm nhánh exception do compiler sinh |
| --- | --- | --- |
| aes_core.cpp | **93/93 (100%)** | 103/120 (85,83%) |
| aes_controller.cpp | **130/130 (100%)** | 185/220 (84,09%) |
| Tổng | **223/223 (100%)** | **288/340 (84,71%)** |

Coverage dòng không đồng nghĩa với bao phủ 100% yêu cầu hoặc mọi corner case.
Ngoài các nhánh exception, còn một nhánh điều kiện reset và một nhánh guard
khởi tạo S-box chưa được đi qua theo gcov. Suite chưa chứng minh mọi lịch xen
kẽ giao dịch, arbitration giữa nhiều master trên fabric, mọi khóa/input,
AXI ở mức tín hiệu hay tương đương RTL. Reset/address/result vẫn theo profile
VP đã công bố do các vấn đề còn mở trong PDF.

## Chạy lại và bằng chứng

Từ thư mục gốc dự án trong Linux/WSL:

```sh
bash components/FU3_Components/AES-256/scripts/run_full.sh
```

Artifact trong workspace:

- `build/fu3-aes/verification-summary.json`: tên test, flags, coverage,
  compiler/OpenSSL, SHA-256 source và vector.
- `build/{fu3-aes,fu3-aes-ubsan,fu3-aes-release}/results.xml`: kết quả JUnit.
- `Testing/Temporary/LastTest.log` trong mỗi build: log và số check từng test.
- `build/fu3-aes/coverage/*.gcov.json.gz`: dữ liệu coverage gốc.
- `openssl-vectors.txt` trong mỗi build: input và expected output độc lập.

SHA-256 corpus đã dùng:
`3608d2225fffd3614282ed17a6ab91d648cdfd35ef6b6ff1425c1b4725e3d3a5`.
Header corpus ghi phiên bản OpenSSL nên hash có thể đổi khi đổi phiên bản.
Chi tiết test và điều kiện build nằm trong [testing.md](testing.md).
