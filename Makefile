# Makefile for pitch_separator and transient_pitch_separator

CC ?= gcc
CFLAGS ?= -O3 -Wall -std=c99 -Ianalysis/offline
LDFLAGS ?= -lm -lpthread

WIN_CC = x86_64-w64-mingw32-gcc
WIN_CFLAGS = -O3 -Wall -std=c99 -Ianalysis/offline -static -Wl,--stack,16777216

TARGET = analysis/pitch_separator
WIN_TARGET = analysis/pitch_separator.exe
SRCS = analysis/pitch_separator.c

TRANSIENT_TARGET = analysis/transient_pitch_separator
TRANSIENT_WIN_TARGET = analysis/transient_pitch_separator.exe
TRANSIENT_SRCS = analysis/transient_pitch_separator.c analysis/offline/cumulative_transience.c

all: $(TARGET) $(TRANSIENT_TARGET) win

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) -lm

$(TRANSIENT_TARGET): $(TRANSIENT_SRCS)
	$(CC) $(CFLAGS) $(TRANSIENT_SRCS) -o $(TRANSIENT_TARGET) $(LDFLAGS)

win: $(WIN_TARGET) $(TRANSIENT_WIN_TARGET)

$(WIN_TARGET): $(SRCS)
	$(WIN_CC) $(WIN_CFLAGS) $(SRCS) -o $(WIN_TARGET) -lm

$(TRANSIENT_WIN_TARGET): $(TRANSIENT_SRCS)
	$(WIN_CC) $(WIN_CFLAGS) $(TRANSIENT_SRCS) -o $(TRANSIENT_WIN_TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(WIN_TARGET) $(TRANSIENT_TARGET) $(TRANSIENT_WIN_TARGET) analysis/*.o analysis/offline/*.o

.PHONY: all win clean
