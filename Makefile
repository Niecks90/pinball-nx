TARGET      := pinball-nx
BUILD       := build
DEVKITPRO   ?= /opt/devkitpro
DEVKITA64   ?= $(DEVKITPRO)/devkitA64
TOOLBIN     := $(DEVKITPRO)/tools/bin
CC          := $(DEVKITA64)/bin/aarch64-none-elf-gcc

SOURCES     := source/main.c \
               src/core/gbc.c src/core/gbc_exec.c src/core/gbc_ppu.c src/core/gbc_apu.c \
               src/core/cpu_a64.S src/game/pinball.c
OBJECTS     := $(patsubst %.c,$(BUILD)/%.o,$(filter %.c,$(SOURCES))) \
               $(patsubst %.S,$(BUILD)/%.o,$(filter %.S,$(SOURCES)))
DEPS        := $(OBJECTS:.o=.d)

ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57
CPPFLAGS    := -Isrc -Isrc/core -Isrc/game -I$(DEVKITPRO)/libnx/include
CFLAGS      := -g -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -ffunction-sections $(ARCH)
LDFLAGS     := -specs=$(DEVKITPRO)/libnx/switch.specs -L$(DEVKITPRO)/libnx/lib -Wl,--gc-sections -Wl,-z,notext
LDLIBS      := -lnx -lm

APP_TITLE   := Pinball NX
APP_AUTHOR  := Niecks90
APP_VERSION := 0.1.0

all: $(TARGET).nro

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@

$(TARGET).nacp:
	$(TOOLBIN)/nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@

$(TARGET).nro: $(BUILD)/$(TARGET).elf $(TARGET).nacp icon.jpg
	$(TOOLBIN)/elf2nro $< $@ --nacp=$(TARGET).nacp --icon=icon.jpg

clean:
	rm -rf $(BUILD) $(TARGET).nro $(TARGET).nacp

-include $(DEPS)
.PHONY: all clean
