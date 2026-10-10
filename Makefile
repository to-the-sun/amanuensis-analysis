# Makefile for pitch_separator

CC ?= gcc
CFLAGS ?= -O3 -Wall -std=c99
LDFLAGS ?= -lm

WIN_CC = x86_64-w64-mingw32-gcc
WIN_CFLAGS = -O3 -Wall -std=c99 -static -Wl,--stack,16777216

TARGET = analysis/pitch_separator
WIN_TARGET = analysis/pitch_separator.exe
SRCS = analysis/pitch_separator.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET) win

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

analysis/%.o: analysis/%.c
	$(CC) $(CFLAGS) -c $< -o $@

win: $(SRCS)
	$(WIN_CC) $(WIN_CFLAGS) $(SRCS) -o $(WIN_TARGET) $(LDFLAGS)

clean:
	rm -f $(OBJS) $(TARGET) $(WIN_TARGET)

.PHONY: all win clean
