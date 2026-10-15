/* SPDX-License-Identifier: Apache-2.0 */
/*
 * FTEL DDR3 controller — CSR register map for firmware (C99, no C++/SystemC needed).
 *
 * Source: HAS V1.0 §8.1 Table 8-5 (offsets, access, reset) and Tables 8-6..8-18 (fields).
 * Offsets are relative to the 64 KiB AXI4-Lite aperture; the SoC base is platform-defined
 * (FX1 VP: FX1_DDR3_CSR_BASE in fx1_memory_map.h).
 *
 * Access rules (HAS §8.1/§8.2): 32-bit, 4-byte-aligned accesses only. W1C and command
 * registers need a full-word write. Reserved "write zero" bits must be written as 0.
 * Registers marked [SNAP] read 0 until the first SNAPSHOT_CMD capture and then show the
 * captured value. Fields from provisional_encodings.h are PROVISIONAL (DEC-13).
 */
#ifndef FX1_DDR3_REGS_H
#define FX1_DDR3_REGS_H

#include "fx1_ddr3/provisional_encodings.h"

#define FX1_DDR3_CSR_SIZE                0x10000u
#define FX1_DDR3_BIT(n)                  (1u << (n))
#define FX1_DDR3_FIELD(v, shift, mask)   (((v) >> (shift)) & (mask))

/* ---------------------------------------------------------------- region 0: global */
#define FX1_DDR3_IP_ID                   0x0000u   /* RO 0x44445233 */
#define FX1_DDR3_ABI_VERSION             0x0004u   /* RO [31:16] major, [15:0] minor */
#define FX1_DDR3_CAP_FEATURE             0x0008u   /* RO */
#define FX1_DDR3_CAP_DEPTH0              0x000Cu   /* RO */
#define FX1_DDR3_CAP_WIDTH0              0x0010u   /* RO */
#define FX1_DDR3_GLOBAL_CTRL             0x0014u   /* RW */
#define FX1_DDR3_GLOBAL_STATUS           0x0018u   /* RO, 0 in this profile */
#define FX1_DDR3_QUIESCE_CTRL            0x001Cu   /* WO-CMD */
#define FX1_DDR3_QUIESCE_STATUS          0x0020u   /* RO */
#define FX1_DDR3_RESET_CAUSE             0x0024u   /* W1C, 0 in this profile */
#define FX1_DDR3_SNAPSHOT_CMD            0x0028u   /* WO-CMD */
#define FX1_DDR3_SNAPSHOT_STATUS         0x002Cu   /* RO */
#define FX1_DDR3_COMMAND_ERROR           0x0030u   /* W1C */
#define FX1_DDR3_SCRATCH                 0x0034u   /* RW */
#define FX1_DDR3_FE_PORT_CMD             0x0040u   /* WO-CMD */
#define FX1_DDR3_FE_PORT_CMD_STATUS      0x0044u   /* RO */
#define FX1_DDR3_FE_PORT_STATE           0x0048u   /* RO */
#define FX1_DDR3_FE_REGION_SHADOW(i)     (0x0100u + 4u * (i))   /* SHADOW, i = 0..31 */
#define FX1_DDR3_FE_REGION_COMMIT        0x0180u   /* WO-CMD */
#define FX1_DDR3_FE_REGION_STATUS        0x0184u   /* RO */
#define FX1_DDR3_FE_REGION_ACTIVE(i)     (0x0200u + 4u * (i))   /* RO, i = 0..31 */
/* Region word index: port p (0..3), region r (0..3), k = 0 BASE / 1 SIZE. */
#define FX1_DDR3_REGION_INDEX(p, r, k)   (2u * (4u * (p) + (r)) + (k))

/* CAP_FEATURE */
#define FX1_DDR3_CAP_FEATURE_ECC         FX1_DDR3_BIT(0)
#define FX1_DDR3_CAP_FEATURE_RATIO(v)    FX1_DDR3_FIELD(v, 1, 0x3u)
#define FX1_DDR3_CAP_FEATURE_RANKS(v)    FX1_DDR3_FIELD(v, 3, 0x3u)
#define FX1_DDR3_CAP_FEATURE_PHY_RANKS(v) FX1_DDR3_FIELD(v, 5, 0x3u)
#define FX1_DDR3_CAP_FEATURE_MAP_MASK(v) FX1_DDR3_FIELD(v, 7, 0x3u)
#define FX1_DDR3_CAP_FEATURE_LIVE_DFI    FX1_DDR3_BIT(9)
#define FX1_DDR3_CAP_FEATURE_RATIO2_PHY  FX1_DDR3_BIT(10)
#define FX1_DDR3_CAP_FEATURE_IRQ_TEST    FX1_DDR3_BIT(11)
/* GLOBAL_CTRL ([31:2] reserved, write zero) */
#define FX1_DDR3_GLOBAL_CTRL_ENABLE      FX1_DDR3_BIT(0)
#define FX1_DDR3_GLOBAL_CTRL_ADM_STOP    FX1_DDR3_BIT(1)
/* QUIESCE_CTRL / QUIESCE_STATUS */
#define FX1_DDR3_QUIESCE_CTRL_REQUEST    FX1_DDR3_BIT(0)
#define FX1_DDR3_QUIESCE_CTRL_RELEASE    FX1_DDR3_BIT(1)
#define FX1_DDR3_QUIESCE_ST_REQUEST      FX1_DDR3_BIT(0)
#define FX1_DDR3_QUIESCE_ST_ADM_STOPPED  FX1_DDR3_BIT(1)
#define FX1_DDR3_QUIESCE_ST_GLOBAL_IDLE  FX1_DDR3_BIT(2)
#define FX1_DDR3_QUIESCE_ST_MISSION_IDLE FX1_DDR3_BIT(3)
#define FX1_DDR3_QUIESCE_ST_DFI_EMPTY    FX1_DDR3_BIT(4)
#define FX1_DDR3_QUIESCE_ST_COMPLETE     FX1_DDR3_BIT(5)
#define FX1_DDR3_QUIESCE_ST_TIMEOUT      FX1_DDR3_BIT(6)
/* SNAPSHOT */
#define FX1_DDR3_SNAPSHOT_CMD_CAPTURE    FX1_DDR3_BIT(0)
#define FX1_DDR3_SNAPSHOT_ST_BUSY        FX1_DDR3_BIT(0)
#define FX1_DDR3_SNAPSHOT_ST_VALID       FX1_DDR3_BIT(1)
#define FX1_DDR3_SNAPSHOT_ST_SEQ(v)      FX1_DDR3_FIELD(v, 16, 0xFFFFu)
/* COMMAND_ERROR */
#define FX1_DDR3_CMD_ERR_BUSY            FX1_DDR3_BIT(0)
#define FX1_DDR3_CMD_ERR_BLOCKED         FX1_DDR3_BIT(1)
#define FX1_DDR3_CMD_ERR_COMMIT          FX1_DDR3_BIT(2)
/* FE_PORT_CMD / status / state (op codes: provisional_encodings.h) */
#define FX1_DDR3_FE_PORT_CMD_VAL(op, port) (((op) & 0x7u) | (((port) & 0x3u) << 8))
#define FX1_DDR3_FE_PORT_ST_BUSY         FX1_DDR3_BIT(0)
#define FX1_DDR3_FE_PORT_ST_DONE         FX1_DDR3_BIT(1)
#define FX1_DDR3_FE_PORT_ST_ERROR        FX1_DDR3_BIT(2)
#define FX1_DDR3_FE_PORT_ST_OP(v)        FX1_DDR3_FIELD(v, 3, 0x7u)
#define FX1_DDR3_FE_PORT_ST_PORT(v)      FX1_DDR3_FIELD(v, 8, 0x3u)
#define FX1_DDR3_FE_PORT_ST_BLOCKER(v)   FX1_DDR3_FIELD(v, 16, 0xFFu)
#define FX1_DDR3_FE_STATE_CSR_QUIESCE(v) FX1_DDR3_FIELD(v, 0, 0xFu)
#define FX1_DDR3_FE_STATE_EFF_QUIESCE(v) FX1_DDR3_FIELD(v, 4, 0xFu)
#define FX1_DDR3_FE_STATE_ACK(v)         FX1_DDR3_FIELD(v, 8, 0xFu)
#define FX1_DDR3_FE_STATE_RESET(v)       FX1_DDR3_FIELD(v, 12, 0xFu)
#define FX1_DDR3_FE_STATE_TIMEOUT(v)     FX1_DDR3_FIELD(v, 16, 0xFu)
#define FX1_DDR3_FE_STATE_RST_PEND(v)    FX1_DDR3_FIELD(v, 20, 0xFu)
#define FX1_DDR3_FE_STATE_DRAINED(v)     FX1_DDR3_FIELD(v, 24, 0xFu)
#define FX1_DDR3_FE_STATE_VALID          FX1_DDR3_BIT(31)
/* FE_REGION_STATUS */
#define FX1_DDR3_FE_REGION_ST_BUSY       FX1_DDR3_BIT(0)
#define FX1_DDR3_FE_REGION_ST_DONE       FX1_DDR3_BIT(1)
#define FX1_DDR3_FE_REGION_ST_ERROR      FX1_DDR3_BIT(2)
#define FX1_DDR3_FE_REGION_ST_BLOCKER(v) FX1_DDR3_FIELD(v, 8, 0xFFu)
#define FX1_DDR3_FE_REGION_ST_VALID      FX1_DDR3_BIT(16)

/* ---------------------------------------------------------------- region 1: topology, timing */
#define FX1_DDR3_TOPO_SHADOW             0x1000u   /* SHADOW */
#define FX1_DDR3_TOPO_ACTIVE             0x1004u   /* RO */
#define FX1_DDR3_USABLE_GRANULES_LO      0x1008u   /* RO [SNAP] */
#define FX1_DDR3_USABLE_GRANULES_HI      0x100Cu   /* RO [SNAP] */
#define FX1_DDR3_PHYSICAL_GRANULES_LO    0x1010u   /* RO [SNAP] */
#define FX1_DDR3_PHYSICAL_GRANULES_HI    0x1014u   /* RO [SNAP] */
#define FX1_DDR3_ECC_BASE_LO             0x1018u   /* RO [SNAP] */
#define FX1_DDR3_ECC_BASE_HI             0x101Cu   /* RO [SNAP] */
#define FX1_DDR3_TOPO_COMMIT             0x1020u   /* WO-CMD */
#define FX1_DDR3_TOPO_COMMIT_STATUS      0x1024u   /* RO */
#define FX1_DDR3_TIMING_SHADOW(i)        (0x1040u + 4u * (i))   /* SHADOW, i = 0..18 */
#define FX1_DDR3_TIMING_COMMIT           0x1090u   /* WO-CMD */
#define FX1_DDR3_TIMING_COMMIT_STATUS    0x1094u   /* RO */
#define FX1_DDR3_TIMING_ACTIVE(i)        (0x1100u + 4u * (i))   /* RO, i = 0..18 */
/* Timing word index (HAS Table 8-12); values in tCK. */
#define FX1_DDR3_T_RCD                   0u
#define FX1_DDR3_T_RP                    1u
#define FX1_DDR3_T_RAS                   2u
#define FX1_DDR3_T_RC                    3u
#define FX1_DDR3_T_RRD                   4u
#define FX1_DDR3_T_FAW                   5u
#define FX1_DDR3_T_CCD                   6u
#define FX1_DDR3_T_RTP                   7u
#define FX1_DDR3_T_WR                    8u
#define FX1_DDR3_T_WTR                   9u
#define FX1_DDR3_T_RTW                   10u
#define FX1_DDR3_T_RTRS                  11u
#define FX1_DDR3_T_RFC                   12u
#define FX1_DDR3_T_REFI                  13u
#define FX1_DDR3_T_CL                    14u
#define FX1_DDR3_T_CWL                   15u
#define FX1_DDR3_T_AL                    16u
#define FX1_DDR3_T_PHY_WRITE_LATENCY     17u
#define FX1_DDR3_T_PHY_READ_LATENCY      18u
#define FX1_DDR3_TIMING_WORDS            19u
/* TOPO_SHADOW / TOPO_ACTIVE */
#define FX1_DDR3_TOPO_VAL(ranks, row_bits, page_code, map) \
    ((((ranks) - 1u) & 1u) | (((row_bits) & 0x1Fu) << 4) | (((page_code) & 0x3u) << 9) | (((map) & 1u) << 12))
#define FX1_DDR3_TOPO_PAGE_256           0u
#define FX1_DDR3_TOPO_PAGE_512           1u
#define FX1_DDR3_TOPO_PAGE_1024          2u
#define FX1_DDR3_TOPO_PAGE_2048          3u
#define FX1_DDR3_TOPO_MAP_LINEAR         0u   /* SOQ-31 (confirm) */
#define FX1_DDR3_TOPO_MAP_INTERLEAVED    1u
/* *_COMMIT_STATUS (topology, timing, DFI) */
#define FX1_DDR3_COMMIT_ST_DONE          FX1_DDR3_BIT(0)
#define FX1_DDR3_COMMIT_ST_ERROR         FX1_DDR3_BIT(1)
#define FX1_DDR3_COMMIT_ST_REINIT        FX1_DDR3_BIT(2)   /* not in DFI_TIMING_STATUS */
#define FX1_DDR3_COMMIT_ST_ACK_BLOCKER(v) FX1_DDR3_FIELD(v, 8, 0xFFu)
#define FX1_DDR3_COMMIT_ST_IMM_BLOCKER(v) FX1_DDR3_FIELD(v, 16, 0xFFu)
#define FX1_DDR3_BLK_TIMING              0x04u   /* MAS */
#define FX1_DDR3_BLK_REGION_NOT_READY    0x0Bu   /* MAS */
#define FX1_DDR3_BLK_REGION_SHAPE        0x0Cu   /* MAS */
#define FX1_DDR3_BLK_REGION_CAPACITY     0x0Du   /* MAS */
#define FX1_DDR3_BLK_TOPOLOGY            0x11u   /* MAS */

/* ---------------------------------------------------------------- region 2: scheduler */
#define FX1_DDR3_WRITE_DRAIN_CFG         0x2000u   /* RW: [7:0] enter, [15:8] exit, [31:16] max */
#define FX1_DDR3_STARVE_LIMIT            0x2004u   /* RW */
#define FX1_DDR3_SCHED_STATUS            0x2008u   /* RO [SNAP], unavailable (reads 0) */
#define FX1_DDR3_STAGED_WRITE_OCC        0x200Cu   /* RO [SNAP], unavailable */
#define FX1_DDR3_WAIT_OLDEST_BE          0x2010u   /* RO [SNAP], unavailable */
#define FX1_DDR3_WAIT_OLDEST_SRT         0x2014u
#define FX1_DDR3_WAIT_OLDEST_HRT         0x2018u
#define FX1_DDR3_SERVICE_COUNT_BE        0x2020u   /* RO [SNAP] */
#define FX1_DDR3_SERVICE_COUNT_SRT       0x2024u
#define FX1_DDR3_SERVICE_COUNT_HRT       0x2028u
#define FX1_DDR3_READ_TIMEOUT            0x2040u   /* RW */
#define FX1_DDR3_DISPATCH_STATUS         0x2044u   /* RO [SNAP], unavailable */
#define FX1_DDR3_READ_META_STATUS        0x2048u   /* RO [SNAP], unavailable */

/* ---------------------------------------------------------------- region 3: refresh, init, power */
#define FX1_DDR3_REFRESH_CFG             0x3000u   /* RW [3:0] debt limit 1..8 */
#define FX1_DDR3_REFRESH_RANK(r)         (0x3004u + 4u * (r))   /* RO [SNAP] */
#define FX1_DDR3_REFRESH_STATUS          0x300Cu   /* RO [SNAP] */
#define FX1_DDR3_INIT_CMD                0x3040u   /* WO-CMD: always SLVERR (init is not software-started) */
#define FX1_DDR3_INIT_STATUS             0x3044u   /* RO [SNAP] */
#define FX1_DDR3_INIT_ERROR              0x3048u   /* W1C */
#define FX1_DDR3_MR(i)                   (0x3050u + 4u * (i))   /* INIT_RW, MR0..MR3 */
#define FX1_DDR3_INIT_RESET_ASSERT_WAIT  0x3060u   /* INIT_RW (readback only) */
#define FX1_DDR3_INIT_RESET_RELEASE_WAIT 0x3064u   /* INIT_RW, controller cycles */
#define FX1_DDR3_INIT_CKE_LOW_WAIT       0x3068u
#define FX1_DDR3_INIT_CKE_HIGH_WAIT      0x306Cu
#define FX1_DDR3_INIT_DLL_WAIT           0x3070u
#define FX1_DDR3_INIT_ZQ_WAIT            0x3074u
#define FX1_DDR3_INIT_CMD_TIMEOUT        0x3078u   /* INIT_RW (stored only) */
#define FX1_DDR3_TRAIN_TRAS_WAIT         0x307Cu
#define FX1_DDR3_POWER_CMD               0x30C0u   /* WO-CMD: always SLVERR (use the P-channel) */
#define FX1_DDR3_POWER_STATUS            0x30C4u   /* RO [SNAP] */
#define FX1_DDR3_POWER_TIMEOUT           0x30C8u   /* RW, nonzero, controller cycles */
#define FX1_DDR3_T_CKSRE                 0x30CCu   /* INIT_RW */
#define FX1_DDR3_T_CKSRX                 0x30D0u   /* INIT_RW */
#define FX1_DDR3_POWER_ERROR             0x30D4u   /* W1C */
#define FX1_DDR3_REFRESH_RANK_DEBT(v)    ((int)((((v) & 0x1Fu) ^ 0x10u)) - 16)   /* signed [4:0] */
#define FX1_DDR3_REFRESH_RANK_URGENCY(v) FX1_DDR3_FIELD(v, 5, 0x3u)
#define FX1_DDR3_INIT_ST_DRAM_INIT       FX1_DDR3_BIT(1)
#define FX1_DDR3_INIT_ST_FAILED          FX1_DDR3_BIT(2)
#define FX1_DDR3_INIT_ST_STAGE(v)        FX1_DDR3_FIELD(v, 7, 0xFu)
#define FX1_DDR3_INIT_ST_ADM_BLOCK       FX1_DDR3_BIT(15)
#define FX1_DDR3_POWER_ST_RESIDENT(v)    FX1_DDR3_FIELD(v, 0, 0x3u)
#define FX1_DDR3_POWER_ST_ACTIVE         FX1_DDR3_BIT(2)
#define FX1_DDR3_POWER_ST_ACCEPT         FX1_DDR3_BIT(3)
#define FX1_DDR3_POWER_ST_DENY           FX1_DDR3_BIT(4)
#define FX1_DDR3_POWER_ST_FATAL_RECOVERY FX1_DDR3_BIT(5)

/* ---------------------------------------------------------------- region 4: ECC, quarantine */
#define FX1_DDR3_ECC_CAP_STATUS          0x4000u   /* RO: [0],[12:9],[20:13] constant; [1],[8:2] [SNAP] */
#define FX1_DDR3_CE_THRESHOLD            0x4004u   /* RW, nonzero */
#define FX1_DDR3_CE_COUNT                0x4008u   /* RO [SNAP] */
#define FX1_DDR3_UE_COUNT                0x400Cu   /* RO [SNAP] */
#define FX1_DDR3_ECC_LAST_ADDR_LO        0x4010u   /* RO [SNAP] */
#define FX1_DDR3_ECC_LAST_ADDR_HI        0x4014u   /* RO [SNAP] */
#define FX1_DDR3_ECC_LAST_INFO           0x4018u   /* RO [SNAP], provisional packing */
#define FX1_DDR3_QUARANTINE_STATUS       0x4040u   /* RO */
#define FX1_DDR3_QUARANTINE_FIRST_REQ_LO 0x4044u   /* RO */
#define FX1_DDR3_QUARANTINE_FIRST_REQ_HI 0x4048u   /* RO */
#define FX1_DDR3_FATAL_STATUS            0x4050u   /* RO sticky, cleared only by reset */
#define FX1_DDR3_BACKEND_OCCUPANCY0      0x4054u   /* RO [SNAP], unavailable */
#define FX1_DDR3_BACKEND_OCCUPANCY1      0x4058u   /* RO [SNAP], unavailable */
#define FX1_DDR3_QUAR_ST_ACTIVE          0x3u      /* [1:0] = 11 when quarantined (MAS) */
#define FX1_DDR3_QUAR_ST_REASON(v)       FX1_DDR3_FIELD(v, 2, 0x7u)
#define FX1_DDR3_QUAR_ST_OPEN_CTX(v)     FX1_DDR3_FIELD(v, 8, 0xFFu)
/* Quarantine reasons, priority 4, 3, 1, 2, 0 (MAS Table 6-135) */
#define FX1_DDR3_QUAR_REASON_TIMEOUT     0x0u      /* head deadline reached */
#define FX1_DDR3_QUAR_REASON_MALFORMED   0x1u      /* malformed return (status 10) */
#define FX1_DDR3_QUAR_REASON_UNEXPECTED  0x2u      /* tokenless or unexpected return */
#define FX1_DDR3_QUAR_REASON_CONTEXT     0x3u      /* no context, or another kind, at pop */
#define FX1_DDR3_QUAR_REASON_GENERATION  0x4u      /* generation mismatch at pop */
/* QUARANTINE_FIRST_REQ_LO (MAS Table 6-137); HI = head deadline [31:0] in tCK */
#define FX1_DDR3_QUAR_FIRST_INDEX(v)     FX1_DDR3_FIELD(v, 0, 0x3Fu)
#define FX1_DDR3_QUAR_FIRST_GEN(v)       FX1_DDR3_FIELD(v, 8, 0xFFFFu)
#define FX1_DDR3_QUAR_FIRST_KIND(v)      FX1_DDR3_FIELD(v, 24, 0x7u)
#define FX1_DDR3_QUAR_FIRST_REASON(v)    FX1_DDR3_FIELD(v, 28, 0x7u)
#define FX1_DDR3_QUAR_FIRST_VALID        FX1_DDR3_BIT(31)
/* FATAL_STATUS[7:3] (MAS §6.2.10/§6.2.15); [2:0] are provisional (SOQ-08) */
#define FX1_DDR3_FATAL_DFI_PROTOCOL      FX1_DDR3_BIT(3)   /* DFI protocol fault hold */
#define FX1_DDR3_FATAL_FETCH_TIMEOUT     FX1_DDR3_BIT(4)   /* write-fetch watchdog (not built) */
#define FX1_DDR3_FATAL_TOPOLOGY_MISMATCH FX1_DDR3_BIT(5)   /* duplicate check (not built) */
#define FX1_DDR3_FATAL_WS_PROTOCOL       FX1_DDR3_BIT(6)   /* write-data protocol fault */
#define FX1_DDR3_FATAL_WR_UNMATCHED      FX1_DDR3_BIT(7)   /* unmatched write result */
/* ECC_CAP_STATUS */
#define FX1_DDR3_ECC_CAP_PRESENT         FX1_DDR3_BIT(0)
#define FX1_DDR3_ECC_CAP_RMW_BUSY        FX1_DDR3_BIT(1)
#define FX1_DDR3_ECC_CAP_LOCKS(v)        FX1_DDR3_FIELD(v, 2, 0x7Fu)

/* ---------------------------------------------------------------- region 5: DFI, PHY, training */
#define FX1_DDR3_PHY_CAP                 0x5000u   /* RO, provider-defined */
#define FX1_DDR3_PHY_STATUS              0x5004u   /* RO [SNAP], provisional packing */
#define FX1_DDR3_DFI_WR_ENABLE_DELAY     0x5010u   /* SHADOW: must equal CWL + AL - PWL - 2 */
#define FX1_DDR3_DFI_RD_ENABLE_DELAY     0x5014u   /* SHADOW: must equal CL + AL + PRL + G - 4 */
#define FX1_DDR3_PHY_MAX_READ_RETURN     0x5018u   /* SHADOW */
#define FX1_DDR3_DFI_ASSEMBLY_MARGIN     0x501Cu   /* SHADOW */
#define FX1_DDR3_DFI_TIMING_COMMIT       0x5020u   /* WO-CMD */
#define FX1_DDR3_DFI_TIMING_STATUS       0x5024u   /* RO */
#define FX1_DDR3_PHY_RDLVL_DELAY         0x5040u   /* INIT_RW [23:0] */
#define FX1_DDR3_PHY_GATE_DELAY          0x5044u   /* INIT_RW [23:0] */
#define FX1_DDR3_PHY_WRLVL_DELAY         0x5048u   /* INIT_RW [2:0] thermometer code */
#define FX1_DDR3_PHY_SKEW_CTRL0          0x504Cu   /* INIT_RW, opaque */
#define FX1_DDR3_PHY_SKEW_CTRL1          0x5050u   /* INIT_RW, opaque */
#define FX1_DDR3_TRAIN_CMD               0x5080u   /* WO-CMD, provisional fields */
#define FX1_DDR3_TRAIN_STATUS            0x5084u   /* RO [SNAP] */
#define FX1_DDR3_TRAIN_TIMEOUT           0x5088u   /* RW, nonzero, controller cycles */
#define FX1_DDR3_TRAIN_OBSERVATION0      0x508Cu   /* RO [SNAP], provisional packing */
#define FX1_DDR3_PHY_COMP_NCTL           0x5090u   /* RO [SNAP] */
#define FX1_DDR3_PHY_COMP_PCTL           0x5094u   /* RO [SNAP] */
#define FX1_DDR3_PHY_COMP_RXCTL          0x5098u   /* RO [SNAP] */
#define FX1_DDR3_PHY_BRIDGE_ERROR        0x509Cu   /* W1C */
#define FX1_DDR3_TRAIN_CMD_VAL(prim, rank, lanes, options) \
    (1u | (((prim) & 0x7u) << 1) | (((rank) & 1u) << FX1_DDR3_TRAIN_CMD_RANK_SHIFT) | \
     (((lanes) & 0xFu) << FX1_DDR3_TRAIN_CMD_LANES_SHIFT) | (((options) & 0xFFFFu) << 12))
#define FX1_DDR3_TRAIN_ST_CODE(v)        FX1_DDR3_FIELD(v, 8, 0xFFu)

/* ---------------------------------------------------------------- region 6: interrupts */
#define FX1_DDR3_IRQ_RAW                 0x6000u   /* RO */
#define FX1_DDR3_IRQ_MASK                0x6004u   /* RW */
#define FX1_DDR3_IRQ_STATUS              0x6008u   /* RO = RAW & MASK */
#define FX1_DDR3_IRQ_CLEAR               0x600Cu   /* W1C (full word) */
#define FX1_DDR3_IRQ_TEST_SET            0x6010u   /* WO-CMD (full word), if CAP_FEATURE[11] */
#define FX1_DDR3_IRQ_CE_THRESHOLD        FX1_DDR3_BIT(0)
#define FX1_DDR3_IRQ_ECC_UE              FX1_DDR3_BIT(1)
#define FX1_DDR3_IRQ_READ_TIMEOUT        FX1_DDR3_BIT(2)
#define FX1_DDR3_IRQ_QUARANTINE          FX1_DDR3_BIT(3)
#define FX1_DDR3_IRQ_MAPPING             FX1_DDR3_BIT(4)
#define FX1_DDR3_IRQ_REFRESH             FX1_DDR3_BIT(5)
#define FX1_DDR3_IRQ_INIT_FAIL           FX1_DDR3_BIT(6)
#define FX1_DDR3_IRQ_TRAIN_FAIL          FX1_DDR3_BIT(7)
#define FX1_DDR3_IRQ_POWER_FAIL          FX1_DDR3_BIT(8)
#define FX1_DDR3_IRQ_PHY                 FX1_DDR3_BIT(9)
#define FX1_DDR3_IRQ_CSR_CMD             FX1_DDR3_BIT(10)
#define FX1_DDR3_IRQ_FATAL               FX1_DDR3_BIT(11)
#define FX1_DDR3_IRQ_ALL                 0xFFFu

#define FX1_DDR3_CSR_WORDS               210u       /* decoded words (HAS §8.1) */

#endif /* FX1_DDR3_REGS_H */
