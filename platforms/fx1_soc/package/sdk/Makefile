# SPDX-License-Identifier: Apache-2.0
#
# VP_FX1_V3.0: the FreeRTOS CLI firmware of the FX1 SoC VP, built without
# CDC-VP or CMake. Run from the SDK root or from this directory:
#
#   make -C sw/bootloader/sources/VP_FX1_V3.0 [TOOLDIR=<toolchain>/bin] [-j]
#
#   TOOLDIR  directory of riscv-none-elf-gcc (default: found on PATH)
#   CROSS    tool prefix (default riscv-none-elf-)
#   OUT      output directory (default sw/bootloader/build/VP_FX1_V3.0)
#
# Outputs in $(OUT): freertos_cli.elf, .dis (disassembly), .map, .sections.txt,
# and the objects in obj/. The image does not depend on where the SDK is
# checked out (-ffile-prefix-map): a rebuild gives the packaged ELF byte for
# byte with the same compiler.
# Same compiler flags, include order and sources as the CDC-VP build that the
# package was tested with (fw/fx1_soc/CMakeLists.txt). Do not change -march /
# -mabi: the VP harts are RV32IMAFC and the BSP, the kernel port and every
# object must agree on ilp32f.

VERSION := VP_FX1_V3.0
HERE    := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
OUT     ?= $(abspath $(HERE)/../../build/$(VERSION))
CROSS   ?= riscv-none-elf-
TOOLDIR ?=
TOOL     = $(if $(TOOLDIR),$(TOOLDIR)/,)$(CROSS)
CC       = $(TOOL)gcc
OBJDUMP  = $(TOOL)objdump
SIZE     = $(TOOL)size

KERNEL  := $(HERE)/third_party/FreeRTOS-Kernel
ARCH    := -march=rv32imafc_zicsr_zifencei -mabi=ilp32f -mcmodel=medany
# --param=min-pagesize=0: MMIO lives at low addresses; without it GCC treats a
# constant address below 4 KiB as null + offset. -Wno-int-to-pointer-cast: the
# upstream RV32 port casts the 64-bit mtimecmp base to a pointer.
CFLAGS  := $(ARCH) -O2 -g -std=c11 -ffreestanding -fno-builtin --param=min-pagesize=0 \
           -ffunction-sections -fdata-sections -Wall -Wextra -Werror \
           -I$(HERE)/bsp -I$(HERE)/include \
           -I$(HERE)/app -I$(KERNEL)/include -I$(KERNEL)/portable/GCC/RISC-V \
           -I$(HERE)/isp_selftest -I$(HERE)/demo \
           -Wno-int-to-pointer-cast
# Source paths in __FILE__ (configASSERT) and debug info relative to this
# directory, so the image does not depend on where the SDK is checked out: a
# rebuild anywhere, with the same compiler release, gives the packaged image
# byte for byte.
CFLAGS  += -ffile-prefix-map=$(HERE)=. -ffile-prefix-map=$(HERE)/=
# Same for the toolchain's own headers (lib/gcc/.../include), whose install
# path would otherwise end up in the debug info.
TOOLROOT := $(patsubst %/bin/,%,$(dir $(realpath $(shell command -v $(CC) 2>/dev/null))))
CFLAGS  += $(if $(TOOLROOT),-ffile-prefix-map=$(TOOLROOT)=riscv-none-elf-gcc)
LDFLAGS := -nostdlib -nostartfiles -Wl,--gc-sections -Wl,--no-warn-rwx-segments

SOURCES := $(HERE)/bsp/crt0.S $(HERE)/bsp/fx1_fw.c \
           $(addprefix $(HERE)/app/,main.c rtos_bsp.c rtos_libc.c cli.c hwcheck.c cmd_fpu.c cmd_dma.c \
                                    cmd_isp.c probe.S) \
           $(KERNEL)/tasks.c $(KERNEL)/queue.c $(KERNEL)/list.c $(KERNEL)/portable/MemMang/heap_4.c \
           $(KERNEL)/portable/GCC/RISC-V/port.c $(KERNEL)/portable/GCC/RISC-V/portASM.S \
           $(HERE)/isp_selftest/isp_common.c $(HERE)/drivers/isp/fx1_isp_drv.c
HEADERS := $(wildcard $(HERE)/bsp/*.h $(HERE)/app/*.h $(HERE)/include/*/*.h $(HERE)/isp_selftest/*.h \
                      $(HERE)/demo/*.h $(KERNEL)/include/*.h $(KERNEL)/portable/GCC/RISC-V/*.h)

ELF  := $(OUT)/freertos_cli.elf
LDS  := $(OUT)/fx1.ld
# One object per source, so no compiler temporary name reaches the symbol table.
OBJS := $(patsubst $(HERE)/%,$(OUT)/obj/%.o,$(SOURCES))

.PHONY: all clean toolchain
all: $(ELF) $(OUT)/freertos_cli.dis $(OUT)/freertos_cli.sections.txt

toolchain:
	@command -v $(CC) >/dev/null 2>&1 || { echo "error: $(CC) not found; set TOOLDIR=<toolchain>/bin" >&2; exit 1; }
	@$(CC) --version | head -n 1

$(LDS): $(HERE)/bsp/fx1.ld.S $(HERE)/include/fx1/fx1_memory_map.h | toolchain
	@mkdir -p $(OUT)
	$(CC) -E -P -x c -DFX1_LINKER_SCRIPT -I$(HERE)/include $< -o $@

$(OUT)/obj/%.o: $(HERE)/% $(HEADERS) | toolchain
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJS) $(LDS)
	$(CC) $(CFLAGS) $(LDFLAGS) -T $(LDS) -Wl,-Map,$(OUT)/freertos_cli.map $(OBJS) -lgcc -o $@

$(OUT)/freertos_cli.dis: $(ELF)
	$(OBJDUMP) -d $< > $@

$(OUT)/freertos_cli.sections.txt: $(ELF)
	$(SIZE) -A -x $< > $@

clean:
	rm -rf $(OUT)/obj
	rm -f $(ELF) $(LDS) $(OUT)/freertos_cli.dis $(OUT)/freertos_cli.map $(OUT)/freertos_cli.sections.txt
