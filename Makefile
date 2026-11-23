TARGET      := pinball-nx
BUILD       := build
SOURCES     := source src/core src/game
INCLUDES    := -Isrc -Isrc/core -Isrc/game
ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57
CFLAGS      := -g -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -ffunction-sections $(ARCH) $(INCLUDES)
ASFLAGS     := -g $(ARCH)
LIBS        := -lnx -lm

APP_TITLE   := Pinball NX
APP_AUTHOR  := Niecks90
APP_VERSION := 1.0.0

include $(DEVKITPRO)/libnx/switch_rules
