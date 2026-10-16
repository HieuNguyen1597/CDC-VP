# Cross-compile the bare-metal hello for the riscv_cpu_eval platform.
#   PLATFORM=cpu_eval (Default): riscv_cpu_eval, RAM 0x8000_0000, UART 0x1000_0000.
#   PLATFORM=fu3: CVA6_ISRAM 0x0001_0000, UART0 0x440D_0000, RV32IMC (RVA = 0,).
# Requires the RISC-V toolchain on PATH (source tools/setup_env.sh).

CROSS   ?= riscv-none-elf-
CC      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump
PLATFORM ?= cpu_eval
NAME     := hello

ifeq ($(PLATFORM),fu3)
ARCH     := -march=rv32imc_zicsr -mabi=ilp32
LDSCRIPT := ../fu3_cva6_smoke/linker.ld
DEFS     := -DUART_BASE=0x440D0000u
TARGET   := $(NAME)_fu3.elf
else ifeq ($(PLATFORM),cpu_eval)
# RV32IMAC / ilp32 matches the mariusmm core (CDC_CPU_BACKEND=riscv_tlm).
ARCH    := -march=rv32imac -mabi=ilp32
LDSCRIPT := linker.ld
DEFS     :=
TARGET   := $(NAME).elf
endif

CFLAGS  := $(ARCH) -Os -ffreestanding -nostdlib -fno-pic -Wall -Wextra  $(DEFS)
LDFLAGS := -T $(LDSCRIPT) -Wl,--gc-sections
SRCS := startup.S src/main.c

all: $(TARGET)

$(TARGET): $(SRCS) $(LDSCRIPT) Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@
	$(OBJDUMP) -d $@ > $(basename $@).dis

clean:
	rm -f $(NAME).elf $(NAME).dis $(NAME)_fu3.elf $(NAME)_fu3.dis

.PHONY: all clean
