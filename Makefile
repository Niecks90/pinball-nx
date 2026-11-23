TARGET      := pinball-nx
BUILD       := build
SOURCES     := source/main.c \
               src/core/gbc.c src/core/gbc_exec.c src/core/gbc_ppu.c src/core/gbc_apu.c \
               src/core/cpu_a64.S src/game/pinball.c
INCLUDES    := -Isrc -Isrc/core -Isrc/game
CFLAGS      := -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter $(INCLUDES)
LDFLAGS     := -lnx -lm

OBJECTS     := $(patsubst %.c,$(BUILD)/%.o,$(filter %.c,$(SOURCES))) \
               $(patsubst %.S,$(BUILD)/%.o,$(filter %.S,$(SOURCES)))

all: $(TARGET).nro

$(BUILD):
	@mkdir -p $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(TARGET).nro: $(BUILD)/$(TARGET).elf $(TARGET).nacp icon.jpg
	elf2nro $< $@ --nacp=$(TARGET).nacp --icon=icon.jpg

$(TARGET).nacp:
	nacptool --create "Pinball NX" "Niecks90" "1.0.0" $@

clean:
	rm -rf $(BUILD) $(TARGET).nro $(TARGET).nacp

-include $(OBJECTS:.o=.d)
.PHONY: all clean
