# Generic Vector Unit (GVU) Top-Level Implementation and Verification Guide

This document describes the cycle-accurate C++ / SystemC model of the Generic Vector Unit top level (`rtl_ref_gvu_top.h`), its dataset-driven testbenches for **Softmax** (`tb_gvu_top.cpp`), **GEMM Fused Requant + Activation** (`tb_gvu_top_gemm_fused.cpp`), and **Element-Wise Residual Add** (`tb_gvu_top_residual_add.cpp`), along with the detailed trace tool (`trace_gvu_top.cpp`). Stage definitions follow the Micro-Architecture Specification (MAS §5.2.1, §5.2.2, §5.2.3, §5.2.6).

> **Status Summary:**
> - **Softmax (`GVU_OP_SOFTMAX`)**: Verified in both `tile` (4,096 elements, cycle 432) and `vec` (64 elements, cycle 223) modes (`tb_gvu_top.cpp`, **RESULT: PASS, 0 mismatches**).
> - **GEMM Fused (`GVU_OP_GEMM_FUSED`)**: Verified in both `tile` (4,096 elements, cycle 144) and `vec` (64 elements, cycle 77) modes (`tb_gvu_top_gemm_fused.cpp`, **RESULT: PASS, 0 mismatches**) using `lut_act.txt`, `input.txt`, and `golden_output.txt`.
> - **Element-Wise Residual Add (`GVU_OP_RESIDUAL`)**: Verified in both `tile` (4,096 elements, cycle 274) and `vec` (64 elements, cycle 140) modes (`tb_gvu_top_residual_add.cpp`, **RESULT: PASS, 0 mismatches**) using `input_a.txt`, `input_b.txt`, `golden_sum.txt`, and `golden_output.txt`.
> - **Connection to the SAURIA core**: a stream mode of GEMM_FUSED (`i_gemm_stream`, one beat per cycle on `i_psum_valid` with its own address, lane mask, scale and shift; nine stages), the rounding-mode, table-reload and 16-bit zero-point inputs, and a last-row lane mask for the residual add were added behind inputs that are off by default, so the runs above are unchanged. The instruction path uses them through `has/gvu_top_bridge.h` (`HAS_GVU_TOP`); see `GVU_TOP_CORE_INTEGRATION.md`, sections 4 and 9.

---

## 1. Architectural Overview

### 1.1 Structural Hierarchy & Dual-Use Design
`rtl_ref_gvu_top.h` connects the GVU memory submodules, the functional-unit lane functions and an internal pipeline controller:

```
                                  +-------------------------------------------------------------+
                                  |                     rtl_ref_gvu::GvuTop                     |
                                  |                                                             |
   +--------------------+         |  +--------------------+             +--------------------+  |
   |   Instruction /    |         |  |   Pipeline FSM     |             |    Vector LSU      |  |
   |   Control Beat     |-------->|  |   (Internal FSM)   |<----------->|    (GvuVlsu)       |  |
   +--------------------+         |  +--------------------+             +--------------------+  |
                                  |            |                                   |            |
   +--------------------+         |            v                                   v            |
   | Systolic Array /   |         |  +--------------------+             +--------------------+  |
   | DMA Scratchpad     |-------->|  |     Stream VRF     |             |  Scratchpad SRAM   |  |
   +--------------------+         |  |    (StreamVrf)     |             |  (GvuScratchpad)   |  |
                                  |  +--------------------+             +--------------------+  |
                                  |            |                                   |            |
                                  |            +-----------------+-----------------+            |
                                  |                              |                              |
                                  |                              v                              |
                                  |  +-------------------------------------------------------+  |
                                  |  |                   Functional Units                    |  |
                                  |  | - Comparator (Vcmp32)         - Adder#1/#2 (adder32)  |  |
                                  |  | - Multiplier Mul#0 (vmul48x40) - Mul#1 (vmul32x32)   |  |
                                  |  | - RoundShift#0 (vroundshift72) - RS#1 (vroundshift64) |  |
                                  |  | - Vector Clamp (clamp_lane)   - LUT Unit (LutUnit)    |  |
                                  |  +-------------------------------------------------------+  |
                                  +-------------------------------------------------------------+
```

1. **Pure C++17 core (`rtl_ref_gvu::GvuTop`)**: `eval(in)` evaluates the combinational outputs of the current cycle without changing state; `tick(in, rstn)` advances one clock edge and returns the outputs of that cycle. Inspection accessors: `state()`, `start_d_cnt()`, `end_d_cnt()`, `scratchpad()`, `stream_vrf()`, `vlut()`, `vlsu()`.
2. **SystemC wrapper (`rtl_ref_gvu::ScGvuTop`)**: compiled when `<systemc.h>` is available. `clock_proc` (on `i_clk.pos()`, async reset on `i_rstn`) advances the core and notifies an internal event; `comb_proc` is the **only** driver of the output ports and is woken by the inputs and by that event. `o_layernorm_lut_init_done` and `o_layernorm_pipe_done` are optional ports (`SC_ZERO_OR_MORE_BOUND`), so a softmax-only bench may leave them unbound.

> [!NOTE]
> The two testbenches are currently built in **standalone C++17 mode**: `<systemc.h>` is not on the include path of the `make` rules, so the SystemC branch is not compiled.

### 1.2 Scratchpad SRAM Organization (MAS Table 5-4, 5-5)
Each bank has two buffers (ping-pong, `i_*_buf_sel`); one serves the accelerator ports. Sizes are per buffer.

| Bank | Word | Depth | Bytes | Accelerator ports |
|---|---|---|---|---|
| `sbank` | 32 bit, dual port | 768 | 3 072 | port 0: int32 word (LayerNorm β); port 1: int8 byte (LayerNorm γ) |
| `vbank#0` | 256 bit, single port | 800 | 25 600 | v32int8 (Softmax input / attention score, LayerNorm input / var / z) |
| `vbank#1` | 1024 bit, single port | 32 | 4 096 | port 0: v32int32 (LUT init source, Softmax σ, LayerNorm S); port 1: v32int16 (LayerNorm w); port 2: v32int8 (Softmax row max, LayerNorm result) |
| `vbank#2` | 1024 bit, single port | 256 | 32 768 | port 0: v32int32 (LayerNorm D_k); port 1: v32int8 (Softmax e_i) |

Read data follows the address by **2 cycles** (SRAM output register + bank pipeline register).

> [!WARNING]
> By default the model copies the RTL write-mask behaviour (`ScratchpadParams::wmask_as_written = true`): mask bit *i* writes bits [i, i+8) instead of byte *i*, so only the low DW/8 + 7 bits of a word can change. See Section 6, issue 1.

### 1.3 Integration with Submodules (`rtl_ref_*`) vs. Internal Logic

| Submodule / Component | Header File | Member | Role |
|---|---|---|---|
| `GvuScratchpad` | `rtl_ref_gvu_scratchpad.h` | `scratchpad_` | **Instantiated**: SRAM banks. `scratchpad_.out(w.sp_in)` / `scratchpad_.tick(w.sp_in)`. |
| `GvuVlsu` | `rtl_ref_gvu_vlsu.h` | `vlsu_` | **Instantiated**: address counters, enables, write masks, port selects. `vlsu_.eval(w.vlsu_in)` / `vlsu_.tick(w.vlsu_in)`. One start address per bank (`i_vbank1_start_addr`, `i_vbank2_start_addr`), address mode `AGU_INCR`. |
| `StreamVrf` | `rtl_ref_gvu_vrf.h` | `stream_vrf_` | **Instantiated**: stream registers `vint64`, `vint32`, `vint16`, `vint8`. `stream_vrf_.eval(w.vrf_in)` / `stream_vrf_.tick(w.vrf_in, i_rstn)`. |
| `LutUnit` | `rtl_ref_gvu_lut.h` | `vlut_` | **Instantiated**: table memories (`LutMem`) and lookup pipeline. `vlut_.eval(w.lut_in)` / `vlut_.tick(w.lut_in, i_rstn)`. |
| `Vcmp32` | `rtl_ref_gvu_fu.h` | `vcmp_` | **Instantiated**: 32-lane comparator for Softmax Stage 1 (`CMP_ACCUMULATE`, `CMP_MAX`). |
| Arithmetic lane functions | `rtl_ref_gvu_fu.h` | direct calls | See table below. |

Which arithmetic is done by the ported FU lane functions and which by plain C++ in `GvuTop`:

| Stage | Ported FU lane functions | Plain C++ arithmetic in `GvuTop` |
|---|---|---|
| Softmax ST1 | `Vcmp32` | — |
| Softmax ST2 | `adder32_lane` (ADD_8 sub, ADD_32 accumulate) | — |
| Softmax ST3 | `vmul48x40_lane` (MB_3X16X8), `vroundshift72_lane`, `vmul32x32_lane` (MA_32X32), `vroundshift64_lane`, `adder32_lane` (ADD_16), `clamp_lane` | — |
| LayerNorm ST1 | — | row sum `ln_s_acc_ += x` |
| LayerNorm ST2 | `adder32_lane` (ADD_32 sub) | `H * x_k`, `D_k^2`, `V` accumulation, var requant (`ln_requant_clamp`) |
| LayerNorm ST3 | `clamp_lane` | offset `u` (`ln_rshift_wide`) |
| LayerNorm ST4 | `clamp_lane` | `r * D_k * M0_div` (i128), shift (`ln_rshift_wide`) |
| LayerNorm ST5 | `clamp_lane` | affine requant (`ln_requant_clamp`) |

#### Components Handled Directly Inside `GvuTop`
1. **Pipeline FSM controller**: `rtl_ref_gvu_ctrl.h` ports only the table-load sequence (`LutInitSequence`), because the delivered RTL controller never leaves `SOFTMAX_ST2`. `GvuTop` therefore implements the full controller (`pipe_ctrl_fsm_`, beat counters `start_d_cnt_` / `end_d_cnt_`, transitions for `SOFTMAX_ST1..ST3` and `LAYERNORM_ST1..ST5`).
2. **LayerNorm scalar registers and helpers**: `ln_s_acc_`, `ln_v_acc_`, `ln_final_s_`, `ln_final_var_`, `ln_final_w_` and the helpers `ln_rshift_wide`, `ln_requant_clamp` (64/128-bit fixed point).

---

## 2. Softmax Pipeline Implementation

### 2.1 Mathematical Formulation & HW Principle (MAS §5.2.3)
$$\text{Softmax}(x_i) = \frac{e^{x_i - \max(x)}}{\sum_{j=1}^H e^{x_j - \max(x)}}$$

Hardware computes the non-negative index $d_i = \text{row\_max} - x_i \ge 0$ and looks it up in an $e^{-x}$ table (`LUT_exp`), so no signed index is needed.

### 2.2 FSM States & Execution Stages

```
   +--------+      +----------------+      +--------------+
   |  IDLE  |----->| LUT_RECIP_INIT |----->| LUT_EXP_INIT |
   +--------+      +----------------+      +--------------+
                                                  |
                                                  v
   +--------+      +----------------+      +--------------+
   |  IDLE  |<-----|  SOFTMAX_ST3   |<-----| SOFTMAX_ST2  |<----- SOFTMAX_ST1
   +--------+      +----------------+      +--------------+
```

Each beat reads one 32-lane row; `i_token_len` (256 in the tests) is the number of beats per stage. Stage boundaries use `softmax_token_last = (end_d_cnt_ == token_len - 1)`.

#### Step 0: LUT Initialization (`LUT_RECIP_INIT`, `LUT_EXP_INIT`)
* Source: `vbank#1` port 0, rows of 1024 bits; a table write follows its read by 2 cycles.
* `LUT_RECIP_INIT` (6 cycles): 4 writes of 64 × 16-bit words → all 256 entries of `LUT_recip`.
* `LUT_EXP_INIT` (2 cycles): 2 writes of 128 bytes. The 7-bit address increment of the table memory is 128 & 0x7F = 0, so **both writes land on entries 0..127** and entries 128..255 are never written (RTL behaviour, see `rtl_ref_gvu_ctrl.h`).
* `o_softmax_lut_init_done` = both tables loaded. The flags persist, so later softmax runs skip the init states.

#### Stage 1: Row Maximum Search (`SOFTMAX_ST1`)
* **Input**: x_i (32 × int8) from `vbank#0`.
* **FU**: `vcmp_` in `CMP_ACCUMULATE` / `CMP_MAX`, signed 8-bit.
* **Writeback**: when the comparator output is valid, `row_max` (32 × int8) is written to `vbank#1` port 2 and the FSM moves to `SOFTMAX_ST2`.

#### Stage 2: Exponential Lookup & Summation (`SOFTMAX_ST2`)
* **Inputs**: x_i from `vbank#0`, `row_max` from `vbank#1` port 2.
* **Adder#2** (`adder32_lane`, `ADD_8`, sub mode): $d_i = \text{row\_max} - x_i$, passed through `vint8[0]` to the LUT.
* **LUT unit**: $e_i = \text{LUT\_exp}[d_i]$.
* **Adder#1** (`adder32_lane`, `ADD_32`): running sum $\sigma = \sum e_j$ held in Stream VRF `vint32[2]`.
* **Writeback**: e_i (32 × int8) to `vbank#2` port 1 on every valid lookup.
* **σ writeback (intended)**: at `softmax_token_last` the code drives σ on `vbank#1` port 0 write data. **This write never reaches the SRAM**: the `vbank#1` port select stays at 2 in `SOFTMAX_ST2`, so the VLSU enables port 2, not port 0 (Section 6, issue 2).

#### Stage 3: Attention Score Normalization (`SOFTMAX_ST3`)
* **Inputs**: σ from `vbank#1` port 0 (the LUT_recip index), e_i from `vbank#2` port 1.
* **LUT unit**: $(r, E_{\text{shift}}) = \text{LUT\_recip}(\sigma)$; E_shift is delayed 3 cycles to line up with the product.
* **Mul#0**: `vmul48x40_lane(MB_3X16X8)`: $e_i \times r$.
* **RoundShift#0**: `vroundshift72_lane` by E_shift (round-nearest-away, 32-bit wrap).
* **Mul#1**: `vmul32x32_lane(MA_32X32)` by `i_attn_scale`.
* **RoundShift#1**: `vroundshift64_lane` by `i_attn_shift` (narrow to 16 bits).
* **Adder#2**: `adder32_lane(ADD_16)` adds `i_attn_z`.
* **Clamp**: `clamp_lane` to signed int8 [−128, 127].
* **Writeback**: attention score (32 × int8) to `vbank#0` when `vint16[5]` is valid.
* **Completion**: `o_softmax_pipe_done` = (`SOFTMAX_ST3` and `softmax_token_last`); the FSM returns to `IDLE`.

Stream VRF registers used: `vint8[0]`, `vint32[2]` (ST2); `vint16[2]`, `vint32[1]`, `vint32[3]`, `vint64[1]`, `vint16[4]`, `vint16[5]` (ST3).

---

## 3. LayerNorm Pipeline Implementation

### 3.1 Mathematical Formulation & HW Optimization (MAS §5.2.5)
$$\mu = \frac{1}{H} \sum_{k=1}^H x_k, \quad \sigma^2 = \frac{1}{H} \sum_{k=1}^H (x_k - \mu)^2, \quad z_k = \frac{x_k - \mu}{\sqrt{\sigma^2 + \epsilon}}, \quad y_k = \gamma_k z_k + \beta_k$$

Hardware avoids dividing by H with the row sum $S = \sum x_k$ and $D_k = H \cdot x_k - S$; the factor 1/H is folded into the requantization multipliers ($M_0$) and shifts ($TS$).

### 3.2 FSM States & Execution Stages

```
   +--------+      +----------------+      +---------------+
   |  IDLE  |----->| LUT_RSQRT_INIT |----->| LAYERNORM_ST1 |
   +--------+      +----------------+      +---------------+
                                                   |
                                                   v
   +--------+      +----------------+      +---------------+
   |  IDLE  |<-----|  LAYERNORM_ST5 |<-----| LAYERNORM_ST4 |<----- LAYERNORM_ST3 <----- LAYERNORM_ST2
   +--------+      +----------------+      +---------------+
```

Stage boundaries use `layernorm_token_last = (end_d_cnt_ >= token_len - 1)`. H = `i_ln_h` if > 0, else `i_token_len`, else 32.

> [!IMPORTANT]
> In the current model **S, var and w are single scalars** shared by all 32 lanes, and **γ and β are one value per beat** shared by all lanes. See Section 6, issues 3 and 6.

#### Step 0: LUT Initialization (`LUT_RSQRT_INIT`)
* 10 cycles: 8 writes of 64 × 16-bit words from `vbank#1` port 0 → all 512 entries of `LUT_rsqrt`.
* `o_layernorm_lut_init_done` stays set; later LayerNorm runs go straight to `LAYERNORM_ST1`.

#### Stage 1: Row Sum (`LAYERNORM_ST1`)
* **Input**: 32 × int8 from `vbank#0` per beat.
* **Operation** (plain C++): `ln_s_acc_` adds **all 32 lanes** of every beat into one scalar.
* **Stream VRF**: the scalar is streamed into `vint32[0]` (same value in every lane).
* **Writeback**: at `layernorm_token_last` the scalar is written to all 32 lanes of `vbank#1` port 0. The value written is the register **before** the last beats are added; in the trace it covers 254 of 256 beats (Section 6, issue 3).

#### Stage 2: Deviation & Variance (`LAYERNORM_ST2`)
* **Inputs**: x_k from `vbank#0`, S from `vbank#1` port 0 (if that read is zero, the internal `ln_final_s_` is used instead).
* **Operation**: `H * x_k` (plain C++), then **Adder#2** `adder32_lane(ADD_32, sub)`: $D_k = H \cdot x_k - S$.
* **Variance** (plain C++): `ln_v_acc_ += D_k^2` over all lanes; at row end $V_{\text{pre}} = V \gg \text{Pre\_Shift}$ and
  $$var = \text{clamp8}(\text{RoundShift}(V_{\text{pre}} \cdot M_{0\_\text{var}}, TS_{\text{var}}) + Z_{\text{var}})$$
* **Writeback**: D_k (32 × int32) to `vbank#2` port 0 every beat (also `vint32[2]`, and D_k² to `vint64[0]`); at row end the scalar var is written to all lanes of `vbank#0`.

#### Stage 3: Offset & Floor (`LAYERNORM_ST3`, 3 cycles)
* **Input**: the scalar var (`ln_final_var_`).
* **Operation**:
  $$u = \text{RoundShift}((var - Z_{\text{var}}) \cdot M_{0\_7} + E_{\text{bias}}, TS_7), \quad f = \text{clamp8}(u + Z_7)$$
  $$w = (f > Z_7) \;?\; (f - Z_7) \;:\; (\texttt{i\_ln\_r8\_floor\_z\_out} \;?\; \texttt{i\_ln\_r8\_z\_out} : 1)$$
* **Writeback**: scalar w (16 bit) to all lanes of `vbank#1` port 1; also `vint8[0]` (var) and `vint16[0]` (w).

#### Stage 4: Normalization (`LAYERNORM_ST4`)
* **Inputs**: w from `vbank#1` port 1 (if zero, `ln_final_w_` is used), D_k from `vbank#2` port 0.
* **LUT unit**: $(r, E) = \text{LUT\_rsqrt}(w)$; r = 1 if the table returns 0.
* **Operation** (plain C++, i128): $P = r \cdot D_k \cdot M_{0\_\text{div}}$ (with `i_ln_r9_exact = false`, the low `i_ln_r9_lsb_bits` are dropped before multiplying by M0_div), then
  $$z_k = \text{clamp8}(\text{RoundShift}(P, TS_{\text{div}} + E) + Z_{\text{div}})$$
  via `clamp_lane`.
* **Writeback**: z_k (32 × int8) to `vbank#0` and `vint8[0]`.

#### Stage 5: Affine Scale and Bias (`LAYERNORM_ST5`)
* **Inputs**: z_k from `vbank#0`; γ = `sbank` port 1 byte (γ = 1 if the byte is 0); β = `sbank` port 0 word. One γ and one β per beat, shared by all lanes.
* **Operation**:
  $$t_k = \text{clamp}(\text{RoundShift}((z_k - Z_{\text{div}}) \cdot \gamma \cdot M_{0\_\text{mul}}, TS_{\text{mul}}) + Z_{\text{mul}}, lo, hi)$$
  $$y_k = \text{clamp}(t_k - Z_{\text{mul}} + \beta + Z_{\text{out}}, lo, hi)$$
  with [lo, hi] = [−128, 127], or [−32768, 32767] when `i_ln_out_int16 = 1`.
* **Writeback**: the low byte of y_k (32 × int8) to `vbank#1` port 2. With `i_ln_out_int16 = 1` the upper byte is dropped.
* **Completion**: `o_layernorm_pipe_done` = (`LAYERNORM_ST5` and `layernorm_token_last`); the FSM returns to `IDLE`. `o_softmax_pipe_done` is asserted in the same cycle as well.

### 3.3 LayerNorm Configuration Parameters (`GvuTop::In`)

| Parameter | C++ type | Default | Role |
|---|---|---|---|
| `i_ln_h` | `int32_t` | 0 | H; 0 → use `i_token_len` (then 32) |
| `i_ln_pre_shift` | `int32_t` | 0 | Pre_Shift applied to V |
| `i_ln_m0_var` | `int64_t` | 1 | M0_var (variance requant) |
| `i_ln_ts_var` | `int32_t` | 0 | TS_var |
| `i_ln_z_var` | `int32_t` | −128 | Z_var |
| `i_ln_m0_7` | `int64_t` | 1 | M0_7 (offset u) |
| `i_ln_ts_7` | `int32_t` | 0 | TS_7 |
| `i_ln_e_bias` | `int64_t` | 0 | E_bias (ε term) |
| `i_ln_z_7` | `int32_t` | −128 | Z_7 |
| `i_ln_m0_div` | `int64_t` | 1 | M0_div (normalization) |
| `i_ln_ts_div` | `int32_t` | 0 | TS_div |
| `i_ln_z_div` | `int32_t` | 0 | Z_div |
| `i_ln_m0_mul` | `int64_t` | 1 | M0_mul (affine) |
| `i_ln_ts_mul` | `int32_t` | 0 | TS_mul |
| `i_ln_z_mul` | `int32_t` | 0 | Z_mul |
| `i_ln_z_out` | `int32_t` | 0 | Z_out |
| `i_ln_r8_floor_z_out` | `bool` | true | Stage 3 floor: use `i_ln_r8_z_out` (else 1) |
| `i_ln_r8_z_out` | `int32_t` | 1 | Stage 3 floor value |
| `i_ln_r9_exact` | `bool` | true | Stage 4: exact product (else drop low bits) |
| `i_ln_r9_lsb_bits` | `int32_t` | 32 | Stage 4: bits dropped when not exact |
| `i_ln_out_int16` | `bool` | false | int16 clamp range instead of int8 |

M0 and TS values are per-tensor scalars. The MAS reference (`has::LnParams`) has per-channel `gamma`, `beta`, `M0_mul` and `TS_mul`; the model has one `M0_mul` / `TS_mul` and reads γ / β from `sbank`. `tb_gvu_top_layernorm.cpp` sets none of these parameters, so the defaults above are used.

## 4. GEMM Fused Requantization & Activation Pipeline Implementation

### 4.1 Mathematical Formulation & HW Mapping (MAS §5.2.1, §5.2.2, Table 5-2)
The fused GEMM pipeline performs post-processing on partial sums directly streamed from the matrix multiplication engine or memory. The pipeline integrates requantization followed by direct lookup table (LUT) activation:

1. **Requantization Formulation:**
   $$P_l = x_{\text{psum}, l} \times M_0$$
   $$Q_l = \text{RoundShift}(P_l, TS)$$
   $$A_l = Q_l + ZP \quad (\text{or } Q_l \text{ if } \texttt{skip\_zp})$$
   $$y_{\text{clamp}, l} = \text{clamp}(A_l, lo, hi)$$
   where $[lo, hi] = [-128, 127]$ (or $[-32768, 32767]$ when $\texttt{out\_int16}$ is set).

2. **LUT Activation:**
   $$y_{\text{act}, l} = \begin{cases} \text{LUT\_act}[y_{\text{clamp}, l} + 128], & \text{if } \neg\texttt{skip\_act} \\ y_{\text{clamp}, l}, & \text{if } \texttt{skip\_act} \end{cases}$$
   The $+128$ offset converts the signed 8-bit clamped integer in $[-128, 127]$ into an unsigned direct 8-bit index $[0, 255]$ into `LUT_ACT`.

### 4.2 Hardware Functional Units & Datapath

| Operation | Functional Unit | Configuration | Role |
|---|---|---|---|
| Scaling | **Mul#1** (`vmul32x32_lane`) | `MA_32X32` | 32-bit signed multiply $x_{\text{psum}} \times M_0$ |
| Narrowing | **RoundShift#1** (`vroundshift64_lane`) | `NW_16` | Round-to-nearest with shift by $TS$, narrowed to 16 bits |
| Zero-Point Addition | **Adder#2** (`adder32_lane`) | `ADD_16` | 16-bit add of zero-point offset $ZP$ |
| Clamping | **Clamp Unit** (`clamp_lane`) | 16-bit signed input | Saturated clamp to $[-128, 127]$ (or $[-32768, 32767]$) |
| Activation | **LUT Unit** (`vlut_.mem.rdata8`) | `LUT_ACT` direct lookup | Direct 256-byte lookup indexed by $y_{\text{clamp}} + 128$ |
| Writeback | **Scratchpad** (`vbank#0`) & Output Ports | Port 0, single-port | Packed 32 × int8 written to `vbank#0`; driven to `o_gemm_fused_data` with `o_gemm_fused_valid = 1` |

### 4.3 FSM States & Execution Flow

```
   +--------+      +--------------+      +---------------+      +--------+
   |  IDLE  |----->| LUT_ACT_INIT |----->| GEMM_FUSED_ST1|----->|  IDLE  |
   +--------+      +--------------+      +---------------+      +--------+
```

1. **Step 0: LUT Initialization (`LUT_ACT_INIT`)**:
   - Condition: Skipped if `o_gemm_fused_lut_init_done` is already high.
   - Source: `vbank#1` port 0, 1024-bit lines.
   - Operations: 2 writes of 128 bytes each (total 256 bytes) load the entire `LUT_act` memory.
   - Completion: Asserts `o_gemm_fused_lut_init_done = 1` and transitions to `GEMM_FUSED_ST1`.
2. **Stage 1: Streaming Processing (`GEMM_FUSED_ST1`)**:
   - Asserts `o_psum_ready = 1` to signal readiness to accept streaming partial sums.
   - On each clock cycle with `i_psum_valid = 1`:
     - Ingests 32 lanes of `i_psum_vec[l]`.
     - Processes elements in parallel across Mul#1 -> RoundShift#1 -> Adder#2 -> Clamp -> LUT_act.
     - Writes the packed 32 × int8 result row into `vbank#0` at address `end_d_cnt_`.
     - Updates Stream VRF `vint64[1]` and drives `o_gemm_fused_data` / `o_gemm_fused_valid`.
     - Increments `end_d_cnt_`.
   - Completion: When `end_d_cnt_ == token_len - 1`, asserts `o_gemm_fused_pipe_done = 1` and returns to `IDLE`.

### 4.4 GEMM Fused Configuration Parameters (`GvuTop::In`)

| Parameter | C++ Type | Default | Description |
|---|---|---|---|
| `i_gemm_scale` | `int32_t` | 1 | Multiplier $M_0$ |
| `i_gemm_shift` | `uint32_t` | 0 | Shift amount $TS$ |
| `i_gemm_zp` | `int32_t` | 0 | Zero-point offset $ZP$ |
| `i_gemm_per_channel` | `bool` | false | Per-channel scaling selection |
| `i_gemm_skip_act` | `bool` | false | Bypass LUT activation lookup if true |
| `i_gemm_skip_zp` | `bool` | false | Bypass zero-point offset addition if true |
| `i_gemm_out_int16` | `bool` | false | Output 16-bit range $[-32768, 32767]$ instead of 8-bit |
| `i_psum_vec` | `int32_t[32]` | {0} | 32-lane streaming input partial sums |
| `i_psum_valid` | `bool` | false | Valid strobe for streaming input |

---

## 5. Element-Wise Residual Add Pipeline Implementation

### 5.1 Mathematical Formulation & HW Mapping (MAS §5.2.6, Fig 5-7, Fig 5-18, Table 5-2, Table 5-4, Table 5-5)
The Element-Wise Residual Add pipeline (`GVU_OP_RESIDUAL` / `GVU_OP_ELEMWISE`) implements 2-stage residual summation across two quantized tensors: Tensor A (preloaded in `vbank#0`) and Tensor B (preloaded in `vbank#1`).

1. **Stage 1: Dual Dequantization and 32-bit Addition:**
   $$D_{A,l} = \text{RoundShift}\Big(\big(A_l - ZP_A\big) \times M_{0,A}, \; TS_A\Big)$$
   $$D_{B,l} = \text{RoundShift}\Big(\big(B_l - ZP_B\big) \times M_{0,B}, \; TS_B\Big)$$
   $$S_l = D_{A,l} + D_{B,l} \quad (\text{stored as 32-bit int32 in } \texttt{vbank\#2} \text{ port 0})$$
   Both dequantized operands $D_{A,l}$ and $D_{B,l}$ are 32-bit signed integers, and their sum $S_l$ is stored as a 1024-bit vector (32 × int32) into `vbank#2`.

2. **Stage 2: Requantization and Output Clamping:**
   $$P_l = S_l \times M_{0,\text{out}}$$
   $$Q_l = \text{RoundShift}\big(P_l, \; TS_{\text{out}}\big)$$
   $$A_l = Q_l + ZP_{\text{out}}$$
   $$Y_l = \text{clamp}(A_l, lo, hi) \quad (\text{stored in } \texttt{vbank\#0} \text{ and driven to } \texttt{o\_residual\_data})$$
   where $[lo, hi] = [-128, 127]$ (or $[-32768, 32767]$ when $\texttt{i\_res\_out\_int16}$ is set).

### 5.2 Hardware Functional Units & Datapath

| Stage | Operation | Functional Unit | Configuration | Role |
|---|---|---|---|---|
| **Stage 1** | Subtract $ZP_A$ | **Adder#1** (`adder32_lane`) | `ADD_16`, sub mode | 16-bit subtraction $(A_l - ZP_A)$ |
| **Stage 1** | Scale $A$ | **Mul#0** (`vmul48x40_lane`) | `MB_40X32` | 40-bit signed multiply $(A_l - ZP_A) \times M_{0,A}$ |
| **Stage 1** | Round-shift $A$ | **RoundShift#0** (`vroundshift72_lane`) | `NW_32` | Round-nearest-up by $TS_A$, narrowed to 32 bits |
| **Stage 1** | Subtract $ZP_B$ | **Adder#2** (`adder32_lane`) | `ADD_16`, sub mode | 16-bit subtraction $(B_l - ZP_B)$ |
| **Stage 1** | Scale $B$ | **Mul#1** (`vmul32x32_lane`) | `MA_32X16` | 32-bit multiply $(B_l - ZP_B) \times M_{0,B}$ |
| **Stage 1** | Round-shift $B$ | **RoundShift#1** (`vroundshift64_lane`) | `NW_32` | Round-nearest-up by $TS_B$, narrowed to 32 bits |
| **Stage 1** | 32-bit Add | **Adder#1** (`adder32_lane`) | `ADD_32` | Dual 32-bit sum $S_l = D_{A,l} + D_{B,l}$ |
| **Stage 1** | Stage 1 Writeback | **Scratchpad** (`vbank#2`) | Port 0, single-port | 1024-bit word (32 × int32) written to `vbank#2` at row `r` |
| **Stage 2** | Scale Sum | **Mul#1** (`vmul32x32_lane`) | `MA_32X32` | 32-bit multiply $S_l \times M_{0,\text{out}}$ |
| **Stage 2** | Narrowing | **RoundShift#1** (`vroundshift64_lane`) | `NW_16` | Round-nearest-up by $TS_{\text{out}}$, narrowed to 16 bits |
| **Stage 2** | Zero-Point Add | **Adder#2** (`adder32_lane`) | `ADD_16` | 16-bit add of $ZP_{\text{out}}$ |
| **Stage 2** | Clamping | **Clamp Unit** (`clamp_lane`) | 16-bit signed | Saturated clamp to $[-128, 127]$ (or $[-32768, 32767]$) |
| **Stage 2** | Output Writeback | **Scratchpad** (`vbank#0`) & Output Ports | Port 0, single-port | Packed 32 × int8 written to `vbank#0`; driven to `o_residual_data` with `o_residual_valid = 1` |

### 5.3 FSM States & Execution Flow

```
   +--------+      +----------------+      +----------------+      +--------+
   |  IDLE  |----->|  RESIDUAL_ST1  |----->|  RESIDUAL_ST2  |----->|  IDLE  |
   +--------+      +----------------+      +----------------+      +--------+
```

1. **Stage 1: Dequantize & Add (`RESIDUAL_ST1`)**:
   - `vbank#0` provides Tensor A row $r$ (v32int8).
   - `vbank#1` port 2 provides Tensor B row $r$ (v32int8 slice from 128-byte SRAM lines holding 4 vectors each). The quadrant selector `b_part_delay_` tracks quadrant alignment across the 2-cycle read delay.
   - Dequantization arithmetic executes in parallel on Mul#0 / RS#0 and Mul#1 / RS#1.
   - 32-bit adder computes sum $S_l$ and writes to `vbank#2` port 0.
   - Counter `start_d_cnt_` tracks issued reads ($0 \dots \text{token\_len} - 1$); unshifted delayed beat counter `end_d_cnt_` tracks write completion.
   - When all beats are committed, asserts `w.residual_st1_done` and transitions to `RESIDUAL_ST2`.
2. **Stage 2: Requantize & Writeback (`RESIDUAL_ST2`)**:
   - Reads intermediate 32-bit sums from `vbank#2` port 0.
   - Ingests sums through Mul#1 -> RS#1 -> Adder#2 -> Clamp Unit.
   - Writes final quantized elements (32 × int8) to `vbank#0` at row $r$.
   - Drives streaming output ports `o_residual_data` and asserts `o_residual_valid = 1`.
   - On final beat completion, asserts `o_residual_pipe_done = 1` and returns to `IDLE`.

### 5.4 Residual Configuration Parameters (`GvuTop::In`)

| Parameter | C++ Type | Default | Description |
|---|---|---|---|
| `i_res_scale_a` | `int32_t` | $0\text{x}40000000$ | Multiplier $M_{0,A}$ for Tensor A (Q30) |
| `i_res_shift_a` | `uint8_t` | 30 | Shift amount $TS_A$ for Tensor A |
| `i_res_zp_a` | `int8_t` | 0 | Zero-point offset $ZP_A$ for Tensor A |
| `i_res_scale_b` | `int32_t` | $0\text{x}40000000$ | Multiplier $M_{0,B}$ for Tensor B (Q30) |
| `i_res_shift_b` | `uint8_t` | 30 | Shift amount $TS_B$ for Tensor B |
| `i_res_zp_b` | `int8_t` | 0 | Zero-point offset $ZP_B$ for Tensor B |
| `i_res_scale_out` | `int32_t` | $0\text{x}40000000$ | Multiplier $M_{0,\text{out}}$ for Output (Q30) |
| `i_res_shift_out` | `uint8_t` | 30 | Shift amount $TS_{\text{out}}$ for Output |
| `i_res_zp_out` | `int8_t` | 0 | Zero-point offset $ZP_{\text{out}}$ for Output |
| `i_res_round` | `RoundMode` | `RND_NEAR_UP` | Rounding mode (nearest-even, nearest-up, etc.) |
| `i_res_out_int16` | `bool` | false | Clamp to int16 $[-32768, 32767]$ instead of int8 |
| `i_res_a_row` | `unsigned` | 0 | Starting row index in `vbank#0` for Tensor A |
| `i_res_b_row` | `unsigned` | 0 | Starting row index in `vbank#1` for Tensor B |
| `i_res_sum_row` | `unsigned` | 0 | Starting row index in `vbank#2` for intermediate sums |

### 5.5 SRAM Capacity & Token Length Considerations
`vbank#1` has `VBANK1_DEPTH = 32` lines of 128 bytes each, holding a maximum of $32 \times 4 = 128$ vectors of 32 bytes (4,096 elements). While `vbank#0` holds 800 rows and `vbank#2` holds 256 rows, `vbank#1` sets the upper bound for single-buffer residual operands to **128 tokens**. The regression testbench `tb_gvu_top_residual_add.cpp` configures `TOKEN_LEN = 128` (4,096 elements), fully utilizing the available capacity of `vbank#1`.

---

## 6. Testbenches & Verification

### 6.1 What the Testbenches Check

All three testbenches (`tb_gvu_top.cpp`, `tb_gvu_top_residual_add.cpp`, `tb_gvu_top_gemm_fused.cpp`) follow the same dataset-driven verification methodology (`[tile|vec]`):
- **`tile` mode (default)**: `num_token = 64, token_len = 64, num_tile = 2, tile_lane = 32` (`4,096` elements)
- **`vec` mode**: `num_token = 1, token_len = 64, num_tile = 1, tile_lane = 1` (`64` elements)

| Testbench | Source | Op | Dataset Files (`target/test/<op>/{tile,vec}/`) | Checks | Result |
|---|---|---|---|---|---|
| Softmax | `tb_gvu_top.cpp` | `GVU_OP_SOFTMAX` | `lut_recip.txt`, `lut_exp.txt`, `input.txt`, `golden_output.txt` | 100% bit-exact comparison of `vbank0` against `golden_output.txt` | **PASS** (`tile`: cycle 432, 0/4096 mismatches; `vec`: cycle 223, 0/64 mismatches) |
| Residual Add | `tb_gvu_top_residual_add.cpp` | `GVU_OP_RESIDUAL` | `input_a.txt`, `input_b.txt`, `golden_sum.txt`, `golden_output.txt` | 100% bit-exact comparison of Stage 1 `vbank2` (`golden_sum.txt`) and Stage 2 `vbank0` (`golden_output.txt`) | **PASS** (`tile`: cycle 274, 0/4096 mismatches; `vec`: cycle 140, 0/64 mismatches) |
| GEMM Fused | `tb_gvu_top_gemm_fused.cpp` | `GVU_OP_GEMM_FUSED` | `lut_act.txt`, `input.txt`, `golden_output.txt` | 100% bit-exact comparison of `vbank0` against `golden_output.txt` | **PASS** (`tile`: cycle 144, 0/4096 mismatches; `vec`: cycle 77, 0/64 mismatches) |

### 6.2 Building and Running Tests

```bash
make tb_gvu_top tb_gvu_top_residual_add tb_gvu_top_gemm_fused
./tb_gvu_top tile
./tb_gvu_top vec
./tb_gvu_top_residual_add tile
./tb_gvu_top_residual_add vec
./tb_gvu_top_gemm_fused tile
./tb_gvu_top_gemm_fused vec
```

### 6.3 Testbench Execution Sequence

**Standalone C++17 mode** (and SystemC `ScGvuTop` mode when `<systemc.h>` is linked):
1. **Load Dataset**: Loads input, LUT (if applicable), and golden output files from `target/test/{softmax,residual_add,gemm_fused}/{tile|vec}/` and preloads the Scratchpad SRAM banks.
2. **Reset**: `tick(in, false)` for 5 cycles (cycles 1–5, 50 ns at a 10 ns clock).
3. **Idle**: one `tick(in, true)` with default inputs (cycle 6).
4. **Stimulus**: set `i_num_tile`, `i_token_len`, `i_gvu_op`, pipeline-specific quantization parameters, buffer selects (`i_vbank1_buf_sel = true` initially for Softmax / GEMM Fused LUT init, switching to `false` when `*_lut_init_done` asserts), and `i_pipeline_en = 1`.
5. **Run loop** from cycle 7: one `tick` per cycle; logs FSM state transitions (`o_gvu_pipe_ctrl_state`), tile counter (`tile_cnt`), and beat counters (`start_d_cnt`, `end_d_cnt`).
6. **End**: on `pipe_done` (+ 15 cycles), verifies 100% of Scratchpad memory contents against the golden files, prints a sample table of the first 8 positions for Lanes 0..3 (`Exp/Act`), and returns exit code 0 on 0 mismatches.

---

## 7. Detailed Trace Tool

`trace_gvu_top.cpp` (in `tools/`, built via `make trace_gvu_top`) applies the same reset and stimulus as the testbenches and logs, **every cycle**, everything `GvuTop` exposes publicly:
* FSM state, `start_d_cnt`, `end_d_cnt`, all status outputs (`o_psum_ready`, `*_lut_init_done`, `*_pipe_done`);
* LUT memory write pointer and counter (`lut_waddr`, `wr_cnt`);
* Stream VRF registers `vint64[1]`, `vint32[1..3]` (valid bit and lane data);
* every Scratchpad row (buffer 0 of all four banks) whose contents change, with the new contents.

```bash
make trace_gvu_top
./trace_gvu_top softmax|gemm_fused|residual [--preload] [--byte-mask]
```

---


## 8. Known Issues & Resolved Model Bugs

### 8.1 Bugs Resolved During GEMM Fused & Residual Add Implementation

1. **Direct Mode LUT Addressing (`rtl_ref_gvu_lut.h`)**:
   - *Issue*: In direct mode (`LUT_ACT`), table entries are 8-bit bytes written across 1024-bit lines (`INIT_DW / DW = 1024 / 8 = 128` bytes per write). The original code used `(w / DW) & 0x7Fu`, which masked 128 with `0x7F`, evaluating to `0`. Consequently, `lut_waddr` never incremented after the first write, leaving entries 128..255 uninitialized.
   - *Fix*: Changed mask to `& 0x1FFu` so 128 is retained and the second write correctly targets entries 128..255.
2. **16-bit Clamp Sign Extension (`rtl_ref_gvu_top.h`)**:
   - *Issue*: In `GEMM_FUSED_ST1`, `Adder#2` operates in `ADD_16` mode and yields a 16-bit word (`w.adder2_r[l] & 0xFFFFu`). The clamp unit was called with `width_bits = 32`. A negative 16-bit intermediate (e.g., `-152` = `0xFF68`) was interpreted as unsigned `+65384` and erroneously clamped to `+127`.
   - *Fix*: Updated `clamp_lane` invocation to specify `width_bits = 16`, ensuring bit 15 is properly sign-extended before clamping to $[-128, 127]$.
3. **Completion Counter Timing Skew on Multi-Stage Pipelines (`rtl_ref_gvu_top.h`)**:
   - *Issue*: In `tick()`, shifting the delay registers (`vbank0_rd_delay_[1] = vbank0_rd_delay_[0]`) before checking `vbank0_rd_delay_[1]` to increment `end_d_cnt_` caused `end_d_cnt_` to advance 1 cycle too early. As a result, `w.residual_st1_done` and `w.residual_st2_done` were asserted when beat 126 was processed, transitioning to the next state and truncating beat 127.
   - *Fix*: Captured `vbank0_valid_beat = vbank0_rd_delay_[1]` and `vbank2_valid_beat = vbank2_rd_delay_[1]` before register shifting, ensuring exact alignment with data writeback and 100% completion of all beats.

### 8.2 RTL Updates in neo_new & Model Conformance

The updated RTL (`neo_new`) resolved several hardware bugs and refined the Softmax datapath, which have now been incorporated into the cycle-accurate reference model (`rtl_ref_gvu_*.h`):

1. **SRAM Byte-Masking Bug Fix (`sram_single_port.sv`, `sram_dual_port.sv`)**:
   - *RTL Change*: Fixed bit vs byte indexing in write mask (`mem[addr][i*8 +: 8] <= wdata[i*8 +: 8]`).
   - *Model Alignment*: `ScratchpadParams::wmask_as_written = false` is now the natural default, ensuring correct byte-level masking across all scratchpad banks.
2. **LUT Memory Write Width (`gvu_lut_mem.sv`)**:
   - *RTL Change*: Widened `word_per_wr` from `logic [6:0]` to `logic [7:0]`, eliminating overflow during 128-word writes.
   - *Model Alignment*: `rtl_ref_gvu_lut.h` `word_per_wr` and `lut_waddr` calculation verified to handle up to 256/512 entries without wrap-around.
3. **Dedicated 32-Lane Clamp Unit (`gvu_vector_clamp32.sv`)**:
   - *RTL Change*: Refactored generic clamp into `gvu_vector_clamp32` with scalar 32-bit bounds (`i_clamp_hi`, `i_clamp_lo`) and per-lane clamping.
   - *Model Alignment*: Added `clamp32_lane` and `vector_clamp32` in `rtl_ref_gvu_fu.h`.
4. **Per-Lane Shift Widths (`gvu_vroundshift*.sv`)**:
   - *RTL Change*: Parameterized `in_shift` as `[LANES-1:0][$clog2(IN_W)-1:0]` across 64-bit and 72-bit round-shift units.
   - *Model Alignment*: `vroundshift_lane` invoked per lane with individual shift amounts.
5. **Scratchpad Subword Address Delay Pipelines (`gvu_scratchpad.sv`)**:
   - *RTL Change*: Added shift registers matching memory read latency (`SBANK_LATENCY`, `VBANK1_LATENCY`, `VBANK2_LATENCY`) for subword address selection (`sbank_word_addr_1_delay`, `vbank1_word_addr_*_delay`, `vbank2_word_addr_1_delay`).
   - *Model Alignment*: Implemented delay queues `sbank_word_addr_1_delay_`, `vbank1_word_addr_*_delay_`, and `vbank2_word_addr_1_delay_` in `rtl_ref_gvu_scratchpad.h`.
6. **VLSU AGU Reset Mechanism (`gvu_vlsu.sv`)**:
   - *RTL Change*: Added `i_agu_addr_reset` input to force immediate reload of AGU internal address registers when entering new pipeline phases.
   - *Model Alignment*: Ported `i_agu_addr_reset` to `GvuVlsu::In` and wired to phase transition detector `(w.fsm_nxt != pipe_ctrl_fsm_)`.
7. **Softmax Multi-Tile Processing & ST3 Datapath Swap (`gvu_pipe_controller.sv`, `gvu_top.sv`)**:
   - *RTL Change*: Added `i_num_tile` input and `tile_cnt` counter; calculated `vbank0_start_addr = tile_cnt * i_token_len`; swapped ST3 multiplier roles (`Mul#1 = MA_16X16` for recip * exp, `Mul#0 = MB_32X32_16X8` for requantization); increased `exp_e_shift_buf` depth to 4; connected `pipe_ctrl_softmax_st3_requant_valid = clamp_valid_out` to trigger `vbank0_wr`.
   - *Model Alignment*: Fully ported multi-tile logic, ST3 datapath swap, 4-stage shift buffer, and writeback trigger into `rtl_ref_gvu_top.h`.

---

## 9. Specification Conformance Matrix

| Feature | MAS Requirement | `rtl_ref_gvu_top.h` Implementation | Status |
|---|---|---|---|
| **Residual Add FSM** | 2 stages without LUT init (§5.2.6, Fig. 5-7, 5-18) | `IDLE` → `RESIDUAL_ST1` → `RESIDUAL_ST2` → `IDLE` | **Conformant & Verified** |
| **Residual Stage 1 (Dequant A+B)** | Dual dequant + 32-bit add into `vbank#2` | Mul#0, Mul#1, RS#0, RS#1, Adder#1, Adder#2 | **Conformant & Verified** |
| **Residual Stage 2 (Requant Output)** | Requant sum into `vbank#0` & stream out | Mul#1 (`MA_32X32`), RS#1 (`NW_16`), Adder#2, Clamp | **Conformant & Verified** |
| **Residual Golden Match** | 100% bit-accurate functional verification | 128 tokens / 4,096 elements verified against golden reference | **PASS (0 mismatches)** |
| **GEMM Fused FSM** | Table init then streaming ST1 (§5.2.1) | `LUT_ACT_INIT` → `GEMM_FUSED_ST1` → `IDLE` | **Conformant** |
| **GEMM Fused Requant** | Mul#1 (`MA_32X32`), RS#1 (`NW_16`), Adder#2 (`ADD_16`), Clamp | Bit-accurate lane pipeline matching MAS §5.2.1 | **Conformant & Verified** |
| **GEMM Fused Activation** | Direct `LUT_act` lookup (256 bytes) | Direct byte lookup `rdata8(LUT_ACT, y_clamp + 128)` | **Conformant & Verified** |
| **GEMM Fused Golden Match** | Functional accuracy | 256 tokens / 8,192 elements verified against golden reference | **PASS (0 mismatches)** |
| **Softmax FSM** | Multi-tile support with 3 stages after LUT init (§5.2.3, Fig. 5-16) | `LUT_RECIP_INIT` → `LUT_EXP_INIT` → `SOFTMAX_ST1..ST3` (iterating over `i_num_tile`) → `IDLE` | **Conformant & Verified** |
| **Softmax ST3 Datapath** | recip × exp, RS#0, scale, RS#1, + zp, clamp | Mul#1 (`MA_16X16`) → RS#0 (72b) → Mul#0 (`MB_32X32_16X8`) → RS#1 (64b) → Adder#2 (`ADD_16`) → Clamp32 → `vbank0` | **Conformant (neo_new)** |
| **Softmax Multi-Tile** | Process `i_num_tile` consecutive tiles | `tile_cnt_` tracking, AGU start addr `tile_cnt_ * token_len` | **Conformant (neo_new)** |
| LayerNorm FSM | 5 stages after table init (§5.2.5, Fig. 5-17) | `LUT_RSQRT_INIT` → `LAYERNORM_ST1..ST5` → `IDLE` | **Conformant** (control flow) |
| LayerNorm rsqrt | $(r, E) = \text{LUT\_rsqrt}(var + \epsilon)$, 512 × 2 B | `LUT_RSQRT` in `vlut_`, all 512 entries loaded | **Conformant** |
| Scratchpad writes | Byte-masked writes | Byte-masked writes (`mem[addr][i*8 +: 8]`) | **Conformant (neo_new)** |


