# DMA0 / AP_DMA — Cấu hình TLM VP

> Bản nháp theo HDS; chưa phải cấu hình đã đối chiếu RTL. `Chưa chốt` không có nghĩa là 0.

Nguồn: `FTEL_CDC_DMA_Hardware_Design_Specification_V1.0.docx.pdf`.

Cover V1.0; running header FU2 V1.1; destination FU3 requires owner confirmation

## Tham số phần cứng

| Tham số | Giá trị |
|---|---|
| `channels` | 8 |
| `channel_fifo_bytes` | 32 |
| `address_bits` | 32 |
| `axi_data_bits` | 32 |
| `axi_id_bits` | 4 |
| `axi_master_id` | 4 |
| `max_read_pending_global` | 4 |
| `max_write_pending_global` | 4 |
| `max_burst_beats` | 16 |
| `burst_types` | ["FIXED", "INCR"] |
| `transfer_unit_bytes` | [1, 2, 4] |
| `transfer_size_bits` | 10 |
| `transfer_size_unit` | bytes |
| `nonzero_transfer_bytes_max` | 1023 |
| `peripheral_ids` | [1, 31] |
| `memory_id` | 0 |
| `peripheral_credit_per_endpoint` | 1 |
| `irq_outputs` | 1 |
| `arbitration` | independent_read_write_weighted_round_robin |
| `clock_domains` | 1 |
| `reset_active_low` | True |

## Ánh xạ địa chỉ

| Tham số | Giá trị |
|---|---|
| `base_address` | Chưa chốt |
| `minimum_aperture_bytes` | 0x1100 |
| `channel_stride` | 0x100 |
| `channel_range` | ["0x0000", "0x07ff"] |
| `reserved_range` | ["0x0800", "0x0fff"] |
| `global_range` | ["0x1000", "0x10ff"] |
| `unmapped_access` | slave_error |

## Thanh ghi mỗi kênh

Địa chỉ = `DMA_BASE + 0x100 × n + offset`, với `n = 0..7`.

| Thanh ghi | Offset | Access | Reset |
|---|---|---|---|
| `CH_CMD_READ_ADDR` | `0x0` | RW | 0x00000000 |
| `CH_CMD_WRITE_ADDR` | `0x4` | RW | 0x00000000 |
| `CH_CMD_TRANSFER_SIZE` | `0x8` | RW | 0x00000000 |
| `CH_CMD_CONTROL` | `0xc` | RW | 0x00000000 |
| `CH_READ_CONFIG` | `0x10` | RW | 0x84010000 |
| `CH_WRITE_CONFIG` | `0x14` | RW | 0x84010000 |
| `CH_MODE_CONFIG` | `0x18` | RW | 0x00000000 |
| `CH_SCHEDULE_CONFIG` | `0x1c` | RW | 0x00000000 |
| `CH_PERIPHERAL_CONFIG` | `0x20` | RW | 0x00000000 |
| `CH_AXI_ATTR_REG` | `0x24` | RW | 0x00000000 |
| `CH_RESTRICTION_STATUS` | `0x2c` | RO | 0x00000000 |
| `CH_READ_OFFSET_STATUS` | `0x30` | RO | 0x00000000 |
| `CH_WRITE_OFFSET_STATUS` | `0x34` | RO | 0x00000000 |
| `CH_FIFO_STATUS` | `0x38` | RO | Model 0x00000020 |
| `CH_OUTSTANDING_STATUS` | `0x3c` | RO | 0x00000000 |
| `CH_ENABLE` | `0x40` | RW | 0x00000001 |
| `CH_START` | `0x44` | WO | 0x00000000 |
| `CH_ACTIVE_STATUS` | `0x48` | RO | 0x00000000 |
| `CH_TRANSFER_COUNT` | `0x50` | RO | 0x00000000 |
| `CH_INTERRUPT_RAW_STATUS` | `0xa0` | RW | 0x00000000 |
| `CH_INTERRUPT_CLEAR` | `0xa4` | WO | 0x00000000 |
| `CH_INTERRUPT_ENABLE` | `0xa8` | RW | 0x00001fff |
| `CH_INTERRUPT_STATUS` | `0xac` | RO | 0x00000000 |

### CH_CMD_READ_ADDR

| Bit | Field | Ý nghĩa |
|---|---|---|
| 31:0 | `address` | Source address |

### CH_CMD_WRITE_ADDR

| Bit | Field | Ý nghĩa |
|---|---|---|
| 31:0 | `address` | Destination address |

### CH_CMD_TRANSFER_SIZE

| Bit | Field | Ý nghĩa |
|---|---|---|
| 9:0 | `bytes` | Byte count; zero-size behavior unresolved; milestone hiện tại từ chối start size=0 |

### CH_CMD_CONTROL

| Bit | Field | Ý nghĩa |
|---|---|---|
| 0 | `set_int` | Completion event enable |
| 1 | `last` | Stop after command |
| 31:2 | `next_address` | Next command address; low two address bits zero |

### CH_READ_CONFIG

| Bit | Field | Ý nghĩa |
|---|---|---|
| 6:0 | `burst_max_bytes` | Maximum burst bytes |
| 21:16 | `tokens` | Consecutive AXI commands before arbiter release |
| 27:24 | `outstanding_limit` | Effective value clamped to 1..4 |
| 30 | `outstanding_enable` | Multi-pending only for memory endpoint |
| 31 | `increment` | Increment endpoint address |

### CH_WRITE_CONFIG

| Bit | Field | Ý nghĩa |
|---|---|---|
| 6:0 | `burst_max_bytes` | Maximum burst bytes |
| 21:16 | `tokens` | Consecutive AXI commands before arbiter release |
| 27:24 | `outstanding_limit` | Effective value clamped to 1..4 |
| 30 | `outstanding_enable` | Multi-pending only for memory endpoint |
| 31 | `increment` | Increment endpoint address |

### CH_MODE_CONFIG

| Bit | Field | Ý nghĩa |
|---|---|---|
| 29:28 | `end_swap` | 0 none; 1 within 16 bits; 2 within 32 bits; 3 reserved |

### CH_SCHEDULE_CONFIG

| Bit | Field | Ý nghĩa |
|---|---|---|
| 31:0 | `compatibility` | Reads zero; writes have no effect |

### CH_PERIPHERAL_CONFIG

| Bit | Field | Ý nghĩa |
|---|---|---|
| 4:0 | `read_id` | 0 memory; 1..31 peripheral |
| 10:8 | `read_delay` | Recovery cycles |
| 20:16 | `write_id` | 0 memory; 1..31 peripheral |
| 26:24 | `write_delay` | Recovery cycles |

### CH_AXI_ATTR_REG

| Bit | Field | Ý nghĩa |
|---|---|---|
| 3:0 | `rd_cache` | Read cache |
| 5:4 | `rd_prot` | Read protection as specified; 2-bit width needs confirmation |
| 7 | `rd_override` | Read override enable |
| 11:8 | `wr_cache` | Write cache |
| 13:12 | `wr_prot` | Write protection as specified; 2-bit width needs confirmation |
| 15 | `wr_override` | Write override enable |

### CH_RESTRICTION_STATUS

| Bit | Field | Ý nghĩa |
|---|---|---|
| 0 | `source_full_fifo` | Source permits full-FIFO burst |
| 1 | `destination_full_fifo` | Destination permits full-FIFO burst |
| 2 | `both_full_fifo` | Both permit full-FIFO burst |
| 5 | `read_multi_pending` | Effective memory read limit > 1 |
| 6 | `write_multi_pending` | Effective memory write limit > 1 |
| 8 | `m2m` | Both endpoints memory |

### CH_READ_OFFSET_STATUS

| Bit | Field | Ý nghĩa |
|---|---|---|
| 31:0 | `constant` | Always zero |

### CH_WRITE_OFFSET_STATUS

| Bit | Field | Ý nghĩa |
|---|---|---|
| 31:0 | `constant` | Always zero |

### CH_FIFO_STATUS

| Bit | Field | Ý nghĩa |
|---|---|---|
| 5:0 | `read_capacity` | Free bytes excluding read reservations |
| 21:16 | `write_capacity` | Received bytes not allocated to write commands |

### CH_OUTSTANDING_STATUS

| Bit | Field | Ý nghĩa |
|---|---|---|
| 3:0 | `read` | Accepted AR awaiting final read response |
| 11:8 | `write` | Accepted AW awaiting B response |

### CH_ENABLE

| Bit | Field | Ý nghĩa |
|---|---|---|
| 0 | `enable` | Zero prevents new grants; accepted work drains |

### CH_START

| Bit | Field | Ý nghĩa |
|---|---|---|
| 0 | `start` | Write one generates start event |

### CH_ACTIVE_STATUS

| Bit | Field | Ý nghĩa |
|---|---|---|
| 0 | `read` | Read active |
| 1 | `write` | Write active |

### CH_TRANSFER_COUNT

| Bit | Field | Ý nghĩa |
|---|---|---|
| 11:0 | `commands` | Completed commands since latest start |
| 19:16 | `events` | Pending completion interrupt events |

### CH_INTERRUPT_RAW_STATUS

| Bit | Field | Ý nghĩa |
|---|---|---|
| 12:0 | `raw` | W1S model policy, reserved bits masked; không tạo command count |

### CH_INTERRUPT_CLEAR

| Bit | Field | Ý nghĩa |
|---|---|---|
| 12:0 | `clear` | Write-one-clear; bit 0 acknowledges one completion event |

### CH_INTERRUPT_ENABLE

| Bit | Field | Ý nghĩa |
|---|---|---|
| 12:0 | `enable` | Interrupt source enable |

### CH_INTERRUPT_STATUS

| Bit | Field | Ý nghĩa |
|---|---|---|
| 12:0 | `masked` | Raw AND enable |

## Thanh ghi global

Offset tính từ DMA_BASE, không cộng thêm `0x1000` lần nữa.

| Thanh ghi | Offset | Access | Reset | Ý nghĩa |
|---|---|---|---|---|
| `CORE_STATUS` | `0x1000` | RO | 0x00000000 | Bits 7:0: channel masked IRQ bitmap |
| `CORE_JOINT_CONFIG` | `0x1030` | RW | 0x00000000 | Compatibility: read zero, write ignored |
| `CORE_PRIORITY_CONFIG` | `0x1038` | RW | 0x00000000 | Bits 2:0 RD top, 3 RD top enable, 6:4 RD high, 7 RD high enable, 10:8 WR top, 11 WR top enable, 14:12 WR high, 15 WR high enable; arbitration effect unresolved |
| `CORE_CLOCK_DIVIDER` | `0x1040` | RW | 0x00000000 | Compatibility: read zero, write ignored |
| `CORE_CHANNEL_START` | `0x1048` | WO | 0x00000000 | Bits 7:0 start bitmap |
| `PERIPHERAL_RX_REQUEST` | `0x1050` | RW | 0x00000000 | Bits 31:1 software RX requests; bit 0 reserved; clear on service |
| `PERIPHERAL_TX_REQUEST` | `0x1054` | RW | 0x00000000 | Bits 31:1 software TX requests; bit 0 reserved; clear on service |
| `CORE_IDLE_STATUS` | `0x10d0` | RO | 0x00000001 | Bit 0: idle |
| `DESIGN_CAPABILITY_STATUS` | `0x10e0` | RO | Model default 0x00000001; integration override | Bits 3:0 = 1 interrupt destination; other bits unresolved |
| `CORE_CAPABILITY_STATUS0` | `0x10f0` | RO | 0x0a602258 | Channels=8; FIFO log2=5; write depth log2=2; read depth field=2; address width=32; bit22=1; size width=10 |
| `CORE_CAPABILITY_STATUS1` | `0x10f4` | RO | Model default 0x00000f17 | Bits 0 watchdog, 1 timeout, 2 tokens, 3 priority, 4 outstanding, 5 wait, 8 independent, 9 peripheral, 10 command-list, 11 endian, 12 clock divider; model assembles implemented feature bits; verify golden RTL |

## Interrupt

| Bit | Nguồn |
|---|---|
| 0 | `command_complete` |
| 1 | `read_slverr` |
| 2 | `write_slverr` |
| 3 | `read_decerr` |
| 4 | `write_decerr` |
| 5 | `fifo_overflow` |
| 6 | `fifo_underflow` |
| 7 | `read_data_timeout` |
| 8 | `read_address_timeout` |
| 9 | `write_response_timeout` |
| 10 | `write_data_timeout` |
| 11 | `write_address_timeout` |
| 12 | `watchdog_timeout` |

## DMA0 peripheral slots

| Tham số | Giá trị |
|---|---|
| `1` | SPI0 |
| `2` | USART0 |
| `3` | USART1 |
| `4` | QSPI |

## Đề xuất model TLM

Phần này là lựa chọn triển khai đề xuất, không phải yêu cầu phần cứng đã chốt.

| Tham số | Giá trị |
|---|---|
| `provenance` | Model design proposal; not hardware requirements |
| `language` | C++17 |
| `framework` | SystemC 2.3.4 / TLM-2.0 |
| `systemc_version` | 2.3.4 — theo yêu cầu người dùng |
| `register_socket` | target_socket |
| `data_socket` | master_socket |
| `reset_port` | reset_n |
| `irq_port` | irq |
| `request_ports` | ["rx_request[31:1]", "tx_request[31:1]"] |
| `clear_ports` | ["rx_clear[31:1]", "tx_clear[31:1]"] |
| `register_access_bytes` | 4 |
| `register_byte_order` | little_endian_proposed |
| `cycle_ns` | Do platform truyền qua constructor peripheral_cycle; không mặc định tần số. Test peripheral dùng 10 ns |
| `register_latency_ns` | Chưa chốt |
| `axi_memory_attributes` | EngineConfig memory/peripheral CACHE/PROT defaults=0; CH_AXI_ATTR_REG overrides per direction |
| `timeout_cycles` | EngineConfig.transaction_timeout là sc_time integration, mặc định 0=tắt; HDS chưa chốt threshold |
| `watchdog_cycles` | EngineConfig.watchdog_timeout là sc_time integration, mặc định 0=tắt |
| `phases` | ["Register/reset/masks and M2M functional LT", "Peripheral handshake, endian, command-chain after ambiguities resolved", "Independent read/write scheduling, FIFO reservations, bounded pending and timing/error tests"] |
| `timing_note` | Sequential b_transport alone cannot validate 4 outstanding transactions. Use concurrent contexts or AT nb_transport for this phase. Pin-phase AXI timeout and RID/BID/RLAST need extensions or a bus adapter. |

## Điểm cần xác nhận

- Confirm FU3 applicability and document revision
- AXI4 claimed in overview vs WID/4-bit LEN AXI3-style in section 2.5
- Theo xác nhận người dùng: engine suy ra unit riêng từng endpoint từ alignment, byte còn lại và giới hạn cấu hình; không thêm field chọn unit. Quy tắc hiện thực cụ thể xem docs/step_04_m2m.md; cần đối chiếu RTL nếu yêu cầu khớp burst từng giao dịch.
- Zero transfer length, zero burst size and zero token behavior
- Người dùng đã xác nhận command-chain: 4 word little-endian source/destination/byte_count/control; static config giữ qua chain; fetch sau drain.
- Model capability1=0xf17 theo feature bits; design capability default1, upper bits integration override; cần golden RTL đối chiếu
- Reset synchronous vs asynchronous; clock period and all timeout thresholds
- Priority register readback vs section 2.9 weighted-only policy
- AXI PROT is 2 bits in register table; confirm mapping to bus protection
- Raw writes hiện W1S/OR; RO write/WO read trả COMMAND_ERROR là policy model cần đối chiếu RTL
- Completion event counter overflow/saturation policy; start while active behavior
- FIFO status hiện suy ra từ buffer: reset read capacity=32, write data=0; đây là lựa chọn functional cần đối chiếu RTL. Restriction full-FIFO permission dùng policy alignment32; pending flags theo limit hiệu lực.
- APB base/slot and IRQ controller routing are integration-defined
- Memory/peripheral default CACHE/PROT values and unaligned read policy

## Ghi chú triển khai

- Burst giới hạn bởi byte còn lại, alignment, 16 beat, biên 4 KiB, FIFO reservation và peripheral credit.
- Completion sau khi mọi write response hoàn tất; disable chặn grant mới và drain giao dịch đã accept.
- Clear interrupt bit 0 acknowledge một completion event; IRQ giữ nếu còn event.
- RX clear sau beat đọc cuối; TX clear sau write response; xung một chu kỳ.
- Peripheral adapter một credit; FLUSH không chuyển dữ liệu; DRLAST không kết thúc transfer.
- Không thêm unit-select field. Engine suy ra unit; layout command-chain 4 word đã được người dùng xác nhận.

## Phạm vi nguồn

| Tham số | Giá trị |
|---|---|
| `features` | PDF pp. 8-26 |
| `parameters` | pp. 31-33 |
| `registers` | pp. 34-48 |
| `programming` | pp. 49-70 |


## Endian policy hiện tại

END_SWAP=0/1/2 tương ứng nhóm 1/2/4 byte, nhóm tính từ byte đầu command và
được giữ xuyên burst/request. Mode1/2 chỉ nhận byte count chia hết nhóm;
mode3 và partial group trả lỗi start trước bus side effect. Đây là policy
functional cần đối chiếu RTL cho grouping theo lane và partial transfer.
Xem [bước 6](docs/step_06_endian.md).

## Trạng thái triển khai hiện tại

Đã bổ sung capability/restriction, raw interrupt W1S, CACHE/PROT và WSTRB metadata,
protocol/FIFO fault reports, 5 phase timers và adapter functional SINGLE/BURST/FLUSH.
Đã sửa MMIO annotated arrival và dispatch gating/zero-time chain livelock.
Functional **33/33 PASS**, UBSan **33/33 PASS** trên SystemC 2.3.4.
Xem [bước 9](docs/step_09_review_and_fixes.md) cho API, chính sách và phạm vi kiểm chứng.

Phase timers cần target/adapter gọi TransferExtension.enter_phase; RAM thông thường
chỉ có default ReadData/WriteResponse. Fault reports kiểm software-visible handling,
không thay thế detector protocol tại bus pins. DTYPE encoding, upper design capability,
restriction full-FIFO condition, reset sync/async và endian partial groups cần RTL
đối chiếu. Snapshot bước7/8 không dùng làm coverage source hiện tại.
