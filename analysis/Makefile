CC ?= gcc
WIN_CC = $(shell which x86_64-w64-mingw32-gcc 2>/dev/null || echo /tmp/llvm-mingw-20240619-ucrt-ubuntu-20.04-x86_64/bin/x86_64-w64-mingw32-gcc)
CFLAGS ?= -O3 -I. -Wall -Wextra
LDFLAGS ?= -lm -lOpenCL -lpthread

TARGET = analyze
WIN_TARGET = analyze.exe
SOURCES = analyze.c exporter.c cumulative_transience.c crossfade.c
OBJECTS = $(SOURCES:.c=.o)

SONG_TARGET = song_builder
SONG_WIN_TARGET = song_builder.exe
SONG_SOURCES = song_builder.c

FLUX_TARGET = flux_separator
FLUX_WIN_TARGET = flux_separator.exe
FLUX_SOURCES = flux_separator.c

all: $(TARGET) $(WIN_TARGET) $(SONG_TARGET) $(SONG_WIN_TARGET) $(FLUX_TARGET) $(FLUX_WIN_TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@ $(LDFLAGS) || $(CC) $(CFLAGS) $(OBJECTS) -o $@ -lm -lpthread

# Windows reserves 1 MB of stack by default, and the report exporter's deep call
# chains overran it; keep a generous reserve on the executable.
WIN_STACK = -Wl,--stack,33554432

$(WIN_TARGET): $(SOURCES)
	$(WIN_CC) $(CFLAGS) $(SOURCES) -o $@ $(WIN_STACK) -lm -lpthread -lOpenCL || $(WIN_CC) $(CFLAGS) $(SOURCES) -o $@ $(WIN_STACK) -lm

$(SONG_TARGET): $(SONG_SOURCES)
	$(CC) $(CFLAGS) $(SONG_SOURCES) -o $@ -lm

$(SONG_WIN_TARGET): $(SONG_SOURCES)
	$(WIN_CC) $(CFLAGS) $(SONG_SOURCES) -o $@ -lm

$(FLUX_TARGET): $(FLUX_SOURCES)
	$(CC) $(CFLAGS) -pthread $(FLUX_SOURCES) -o $@ -lm -lpthread

$(FLUX_WIN_TARGET): $(FLUX_SOURCES)
	$(WIN_CC) $(CFLAGS) -pthread $(FLUX_SOURCES) -o $@ -lm

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET) $(WIN_TARGET) $(SONG_TARGET) $(SONG_WIN_TARGET) $(FLUX_TARGET) $(FLUX_WIN_TARGET)

.PHONY: all clean
