CC = gcc
WIN_CC = $(shell which x86_64-w64-mingw32-gcc 2>/dev/null || echo /tmp/llvm-mingw-20240619-ucrt-ubuntu-20.04-x86_64/bin/x86_64-w64-mingw32-gcc)
CFLAGS = -O3 -I. -Wall -Wextra
LDFLAGS = -lm -lOpenCL -lpthread

TARGET = ct_analyze_headless
WIN_TARGET = ct_analyze_headless.exe
SOURCES = ct_analyze_headless.c ct_exporter.c cumulative_transience.c crossfade.c
OBJECTS = $(SOURCES:.c=.o)

SONG_TARGET = ct_song_builder
SONG_WIN_TARGET = ct_song_builder.exe
SONG_SOURCES = ct_song_builder.c

all: $(TARGET) $(WIN_TARGET) $(SONG_TARGET) $(SONG_WIN_TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@ $(LDFLAGS) || $(CC) $(CFLAGS) $(OBJECTS) -o $@ -lm -lpthread

$(WIN_TARGET): $(SOURCES)
	$(WIN_CC) $(CFLAGS) $(SOURCES) -o $@ -lm -lpthread -lOpenCL || $(WIN_CC) $(CFLAGS) $(SOURCES) -o $@ -lm

$(SONG_TARGET): $(SONG_SOURCES)
	$(CC) $(CFLAGS) $(SONG_SOURCES) -o $@ -lm

$(SONG_WIN_TARGET): $(SONG_SOURCES)
	$(WIN_CC) $(CFLAGS) $(SONG_SOURCES) -o $@ -lm

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET) $(TARGET).exe $(SONG_TARGET) $(SONG_WIN_TARGET)

.PHONY: all clean
