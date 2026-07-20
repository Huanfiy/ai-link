# Bare-metal bootloader for ailink (flash sectors 0-3, 64 KB).
# Standalone build, no RT-Thread / SCons involvement:
#
#   make -C bootloader [RTT_EXEC_PATH=...]
#
# Reuses the same GNU Arm toolchain as the app build; run.sh build-boot wraps
# this with its RTT_EXEC_PATH default.

RTT_EXEC_PATH ?= $(HOME)/toolchain/arm-eabi-toolchain/bin
CROSS := $(RTT_EXEC_PATH)/arm-none-eabi-

CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

BUILD  := build
TARGET := $(BUILD)/ailink-boot

SRCS := startup.c main.c crc32.c uart.c
OBJS := $(SRCS:%.c=$(BUILD)/%.o)

# fw_info.h is shared with the app (single source of truth for the layout)
INCLUDES := -I. -I../applications/ota

CPUFLAGS := -mthumb -mcpu=cortex-m4 -mfloat-abi=soft
CFLAGS := $(CPUFLAGS) $(INCLUDES) -Os -g -Wall -Wextra -Werror \
          -ffunction-sections -fdata-sections -ffreestanding -MMD -MP
LDFLAGS := $(CPUFLAGS) -T link.lds -nostdlib -Wl,--gc-sections \
           -Wl,-Map=$(TARGET).map,-cref

.PHONY: all clean

all: $(TARGET).bin
	@$(SIZE) $(TARGET).elf

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(TARGET).elf: $(OBJS) link.lds
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
