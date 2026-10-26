# Makefile for pitch separators

CC ?= gcc
CFLAGS ?= -O3 -Wall -std=c99 -I.
LDFLAGS ?= -lm

WIN_CC = x86_64-w64-mingw32-gcc
WIN_CFLAGS = -O3 -Wall -std=c99 -I. -static -Wl,--stack,16777216

TARGET_PITCH = analysis/pitch_separator
WIN_TARGET_PITCH = analysis/pitch_separator.exe
SRCS_PITCH = analysis/pitch_separator.c

TARGET_CT = analysis/ct_pitch_separator
WIN_TARGET_CT = analysis/ct_pitch_separator.exe
SRCS_CT = analysis/ct_pitch_separator.c analysis/offline/cumulative_transience.c

TARGET_FLUX = analysis/flux_pitch_separator
WIN_TARGET_FLUX = analysis/flux_pitch_separator.exe
SRCS_FLUX = analysis/flux_pitch_separator.c

TARGETS = $(TARGET_PITCH) $(TARGET_CT) $(TARGET_FLUX)
WIN_TARGETS = $(WIN_TARGET_PITCH) $(WIN_TARGET_CT) $(WIN_TARGET_FLUX)

all: $(TARGETS) win

$(TARGET_PITCH): $(SRCS_PITCH)
	$(CC) $(CFLAGS) $(SRCS_PITCH) -o $(TARGET_PITCH) $(LDFLAGS)

$(TARGET_CT): $(SRCS_CT)
	$(CC) $(CFLAGS) $(SRCS_CT) -o $(TARGET_CT) $(LDFLAGS)

$(TARGET_FLUX): $(SRCS_FLUX)
	$(CC) $(CFLAGS) -pthread $(SRCS_FLUX) -o $(TARGET_FLUX) $(LDFLAGS)

win: $(WIN_TARGETS)

$(WIN_TARGET_PITCH): $(SRCS_PITCH)
	$(WIN_CC) $(WIN_CFLAGS) $(SRCS_PITCH) -o $(WIN_TARGET_PITCH) $(LDFLAGS)

$(WIN_TARGET_CT): $(SRCS_CT)
	$(WIN_CC) $(WIN_CFLAGS) $(SRCS_CT) -o $(WIN_TARGET_CT) $(LDFLAGS)

$(WIN_TARGET_FLUX): $(SRCS_FLUX)
	$(WIN_CC) $(WIN_CFLAGS) -pthread $(SRCS_FLUX) -o $(WIN_TARGET_FLUX) $(LDFLAGS)

clean:
	rm -f $(TARGETS) $(WIN_TARGETS) analysis/*.o analysis/offline/*.o

.PHONY: all win clean
