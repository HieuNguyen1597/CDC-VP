/* SPDX-License-Identifier: Apache-2.0 */
/*
 * FTEL DDR3 controller — PROVISIONAL field encodings (DEC-13).
 *
 * HAS V1.0 names these fields but neither HAS nor MAS V0.1 defines their encodings
 * (the MAS register chapter is missing). Until the specification is completed, the VP
 * uses the proposals sent to the spec owner (docs/spec_owner_questions.md, Part A).
 * Every value here may change; firmware must use these macros, never the numbers.
 * This file is C99 and C++ compatible and is included by fx1_ddr3_regs.h.
 */
#ifndef FX1_DDR3_PROVISIONAL_ENCODINGS_H
#define FX1_DDR3_PROVISIONAL_ENCODINGS_H

/* SOQ-01: INIT_STATUS[10:7] initialization stage. */
#define FX1_DDR3_INIT_STAGE_HOLD               0x0u
#define FX1_DDR3_INIT_STAGE_PHY_RESET          0x1u
#define FX1_DDR3_INIT_STAGE_PHY_READY_WAIT     0x2u
#define FX1_DDR3_INIT_STAGE_RESET_RELEASE_WAIT 0x3u
#define FX1_DDR3_INIT_STAGE_CKE_LOW_WAIT       0x4u
#define FX1_DDR3_INIT_STAGE_CKE_HIGH_WAIT      0x5u
#define FX1_DDR3_INIT_STAGE_MR2                0x6u
#define FX1_DDR3_INIT_STAGE_MR3                0x7u
#define FX1_DDR3_INIT_STAGE_MR1                0x8u
#define FX1_DDR3_INIT_STAGE_MR0                0x9u
#define FX1_DDR3_INIT_STAGE_DLL_WAIT           0xAu
#define FX1_DDR3_INIT_STAGE_ZQCL               0xBu
#define FX1_DDR3_INIT_STAGE_ZQ_WAIT            0xCu
#define FX1_DDR3_INIT_STAGE_TRAINING           0xDu
#define FX1_DDR3_INIT_STAGE_DONE               0xEu
#define FX1_DDR3_INIT_STAGE_FAILED             0xFu

/* SOQ-02: INIT_ERROR[15:0] one-hot: bit n = stage code n active at the failure. */
#define FX1_DDR3_INIT_ERROR_STAGE(stage)       (1u << (stage))

/* SOQ-03: TRAIN_CMD fields and TRAIN_STATUS codes. */
#define FX1_DDR3_TRAIN_PRIM_LOAD_RDLVL         0x0u
#define FX1_DDR3_TRAIN_PRIM_LOAD_GATE          0x1u
#define FX1_DDR3_TRAIN_PRIM_LOAD_WRLVL         0x2u
#define FX1_DDR3_TRAIN_PRIM_LOAD_ALL           0x3u   /* 4..7 reserved: SLVERR */
#define FX1_DDR3_TRAIN_CMD_RANK_SHIFT          4u     /* TRAIN_CMD[4] rank */
#define FX1_DDR3_TRAIN_CMD_LANES_SHIFT         5u     /* TRAIN_CMD[8:5] byte-lane mask */
#define FX1_DDR3_TRAIN_CMD_LANES_MASK          (0xFu << 5)
#define FX1_DDR3_TRAIN_CMD_RSVD_MASK           (0x7u << 9)  /* TRAIN_CMD[11:9] write zero */
#define FX1_DDR3_TRAIN_STATUS_BUSY             (1u << 0)
#define FX1_DDR3_TRAIN_STATUS_DONE             (1u << 1)
#define FX1_DDR3_TRAIN_STATUS_ERROR            (1u << 2)
#define FX1_DDR3_TRAIN_CODE_OK                 0x00u
#define FX1_DDR3_TRAIN_CODE_TIMEOUT            0x01u
#define FX1_DDR3_TRAIN_CODE_STALE              0x02u
#define FX1_DDR3_TRAIN_CODE_PHY_FAIL           0x03u
#define FX1_DDR3_TRAIN_CODE_REJECTED           0x04u
#define FX1_DDR3_TRAIN_CODE_CANCELLED          0x05u  /* a reset retired the command */
/* TRAIN_OBSERVATION0 packing of the BE09 observation. */
#define FX1_DDR3_TRAIN_OBS_RDLVL_SHIFT         0u     /* [7:0]   read-level response */
#define FX1_DDR3_TRAIN_OBS_WRLVL_SHIFT         8u     /* [15:8]  write-level response */
#define FX1_DDR3_TRAIN_OBS_RDMODE_SHIFT        16u    /* [17:16] read-level mode */
#define FX1_DDR3_TRAIN_OBS_GATEMODE_SHIFT      18u    /* [19:18] gate mode */
#define FX1_DDR3_TRAIN_OBS_WRMODE_SHIFT        20u    /* [21:20] write-level mode */
#define FX1_DDR3_TRAIN_OBS_RANK_SHIFT          24u    /* [24]    rank of the observation */

/* SOQ-04: POWER_ERROR[23:0] bitmap. */
#define FX1_DDR3_POWER_ERR_DENIED_BUSY         (1u << 0)
#define FX1_DDR3_POWER_ERR_DENIED_ILLEGAL      (1u << 1)
#define FX1_DDR3_POWER_ERR_TIMEOUT_PRE_CUTOFF  (1u << 2)
#define FX1_DDR3_POWER_ERR_TIMEOUT_POST_CUTOFF (1u << 3)
#define FX1_DDR3_POWER_ERR_WAKE_PHY_NOT_READY  (1u << 4)
#define FX1_DDR3_POWER_ERR_DENIED_COMMIT_BLOCK (1u << 5)
/* SOQ-04 (extension): P-channel PSTATE / POWER_STATUS[1:0] resident-state encoding. */
#define FX1_DDR3_PSTATE_ACTIVE                 0x0u
#define FX1_DDR3_PSTATE_ACTIVE_POWER_DOWN      0x1u
#define FX1_DDR3_PSTATE_PRECHARGE_POWER_DOWN   0x2u
#define FX1_DDR3_PSTATE_SELF_REFRESH           0x3u

/* SOQ-05: PHY_BRIDGE_ERROR[7:0] bitmap. */
#define FX1_DDR3_PHY_BRIDGE_ERR_TIMEOUT        (1u << 0)
#define FX1_DDR3_PHY_BRIDGE_ERR_STALE          (1u << 1)
#define FX1_DDR3_PHY_BRIDGE_ERR_PROVIDER_FAIL  (1u << 2)
#define FX1_DDR3_PHY_BRIDGE_ERR_NOT_READY      (1u << 3)

/* SOQ-06: ECC_LAST_INFO packing; ECC_LAST_ADDR = physical data granule index of the
 * last CE or UE event (last event wins). */
#define FX1_DDR3_ECC_INFO_TYPE_CE              0x1u   /* [1:0] */
#define FX1_DDR3_ECC_INFO_TYPE_UE              0x2u
#define FX1_DDR3_ECC_INFO_CODEWORD_SHIFT       2u     /* [3:2] lowest affected codeword */
#define FX1_DDR3_ECC_INFO_SYNDROME_SHIFT       8u     /* [15:8] {mismatch, syndrome[6:0]} of it */

/* SOQ-07: QUARANTINE_FIRST_REQ_LO[26:24] token kind (GAP-BE-14). The reason codes and
 * the record layout are MAS-defined (fx1_ddr3_regs.h). */
#define FX1_DDR3_QUAR_KIND_RD_DATA             0x0u   /* read without ECC check */
#define FX1_DDR3_QUAR_KIND_RD_ECC              0x1u   /* ECC-checked read */
#define FX1_DDR3_QUAR_KIND_RMW                 0x2u   /* internal read of an ECC write */

/* SOQ-08: FATAL_STATUS[2:0] ([7:3] are MAS-defined). */
#define FX1_DDR3_FATAL_ECC_HOLD                (1u << 0)
#define FX1_DDR3_FATAL_SCRUB                   (1u << 1)
#define FX1_DDR3_FATAL_SEQUENCE                (1u << 2)

/* SOQ-09: commit blocker codes beyond 0x04 / 0x11 / 0x0B..0x0D (VP-local range). */
#define FX1_DDR3_BLK_NOT_QUIESCED              0xE0u
#define FX1_DDR3_BLK_NO_QUIESCE_ACK            0xE1u
#define FX1_DDR3_BLK_CANCELLED                 0xE2u  /* reset cancelled an accepted transaction */
#define FX1_DDR3_BLK_RECOVERY_REQUIRED         0xE3u
#define FX1_DDR3_BLK_FIELD_ILLEGAL             0xE4u  /* DFI image validation */
#define FX1_DDR3_BLK_DEPENDENCY                0xE5u  /* DFI commit before an active timing image */
#define FX1_DDR3_BLK_BAD_PORT_OR_OP            0xE6u

/* SOQ-10: FE_PORT_CMD op codes (1 and 3 are MAS-defined). */
#define FX1_DDR3_FE_OP_QUIESCE                 0x1u
#define FX1_DDR3_FE_OP_RELEASE                 0x2u
#define FX1_DDR3_FE_OP_SOFT_RESET              0x3u

/* SOQ-11: REFRESH_RANKn[6:5] urgency and REFRESH_STATUS[4:0] bits. */
#define FX1_DDR3_REFRESH_URG_BACKGROUND        0x0u
#define FX1_DDR3_REFRESH_URG_NORMAL            0x1u
#define FX1_DDR3_REFRESH_URG_HIGH              0x2u
#define FX1_DDR3_REFRESH_URG_MANDATORY         0x3u
#define FX1_DDR3_REFRESH_STATUS_R0_MANDATORY   (1u << 0)
#define FX1_DDR3_REFRESH_STATUS_R1_MANDATORY   (1u << 1)
#define FX1_DDR3_REFRESH_STATUS_ANY_HIGH       (1u << 2)
#define FX1_DDR3_REFRESH_STATUS_ANY_MANDATORY  (1u << 3)
#define FX1_DDR3_REFRESH_STATUS_SATURATION     (1u << 4)

/* SOQ-12: PHY_STATUS packing of the synchronized PHY levels. */
#define FX1_DDR3_PHY_STATUS_RESET_DONE         (1u << 0)
#define FX1_DDR3_PHY_STATUS_READY              (1u << 1)
#define FX1_DDR3_PHY_STATUS_FAILURE            (1u << 2)
#define FX1_DDR3_PHY_STATUS_TIMING_WAIT_OK     (1u << 3)

/* SOQ-13: QoS class codes of the sideband table (NORMAL = 1 is derivable from MAS). */
#define FX1_DDR3_QOS_CLASS_BULK                0x0u
#define FX1_DDR3_QOS_CLASS_NORMAL              0x1u
#define FX1_DDR3_QOS_CLASS_LATENCY             0x2u
#define FX1_DDR3_QOS_CLASS_HRT                 0x3u
/* SOQ-36: SERVICE_COUNT_BE counts classes 0 and 1, _SRT class 2, _HRT class 3 (backend
 * service classes are not mapped to the 2-bit frontend codes by either document). */

#endif /* FX1_DDR3_PROVISIONAL_ENCODINGS_H */
