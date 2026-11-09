CC ?= gcc
WIN_CC = $(shell which x86_64-w64-mingw32-gcc 2>/dev/null || echo /tmp/llvm-mingw-20240619-ucrt-ubuntu-20.04-x86_64/bin/x86_64-w64-mingw32-gcc)
CFLAGS ?= -O3 -Wall -Wextra -I.
LIBS = -lm -lpthread
WIN_STACK = -Wl,--stack,33554432

all: analyze_palettes.exe

analyze_palettes.exe: analyze_palettes.c dr_wav.h templates.h
	$(WIN_CC) $(CFLAGS) $< -o $@ $(WIN_STACK) $(LIBS)

analyze_palettes: analyze_palettes.c dr_wav.h templates.h
	$(CC) $(CFLAGS) $< -o $@ $(LIBS)

clean:
	rm -f analyze_palettes analyze_palettes.exe

.PHONY: all clean
