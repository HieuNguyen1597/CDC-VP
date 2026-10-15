/* SPDX-License-Identifier: Apache-2.0 */
/* The firmware register header must build as plain C99 with -Wall -Wextra -Werror. */
#include "fx1_ddr3/fx1_ddr3_regs.h"

#include <stdint.h>

typedef char check_ip_id[(FX1_DDR3_IP_ID == 0x0000u) ? 1 : -1];
typedef char check_init_cmd[(FX1_DDR3_INIT_CMD == 0x3040u) ? 1 : -1];
typedef char check_region[(FX1_DDR3_FE_REGION_SHADOW(31) == 0x017Cu) ? 1 : -1];
typedef char check_timing[(FX1_DDR3_TIMING_ACTIVE(18) == 0x1148u) ? 1 : -1];
typedef char check_topo[(FX1_DDR3_TOPO_VAL(2, 16, FX1_DDR3_TOPO_PAGE_1024, FX1_DDR3_TOPO_MAP_LINEAR) == 0x501u) ? 1 : -1];
typedef char check_words[(FX1_DDR3_CSR_WORDS == 210u) ? 1 : -1];

int main(void) {
    volatile uint32_t v = FX1_DDR3_TRAIN_CMD_VAL(FX1_DDR3_TRAIN_PRIM_LOAD_ALL, 1u, 0xFu, 0u);
    return (FX1_DDR3_REFRESH_RANK_DEBT(0x1Fu) == -1 && FX1_DDR3_REFRESH_RANK_DEBT(0x08u) == 8 && v == 0x1F7u) ? 0 : 1;
}
