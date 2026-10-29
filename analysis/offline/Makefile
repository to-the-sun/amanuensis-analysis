CC = gcc
WIN_CC = x86_64-w64-mingw32-gcc
CFLAGS = -O3 -I. -Wall -Wextra
LDFLAGS = -lm -lOpenCL -lpthread

TARGET = ct_analyze_headless
WIN_TARGET = ct_analyze_headless.exe
SONG_TARGET = ct_song_builder
WIN_SONG_TARGET = ct_song_builder.exe

SOURCES = ct_analyze_headless.c ct_exporter.c cumulative_transience.c crossfade.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET) $(WIN_TARGET) $(SONG_TARGET) $(WIN_SONG_TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@ $(LDFLAGS) || $(CC) $(CFLAGS) $(OBJECTS) -o $@ -lm -lpthread

$(WIN_TARGET): $(SOURCES)
	$(WIN_CC) $(CFLAGS) $(SOURCES) -o $@ -lm -lpthread -lOpenCL || $(WIN_CC) $(CFLAGS) $(SOURCES) -o $@ -lm

$(SONG_TARGET): ct_song_builder.c
	$(CC) $(CFLAGS) $< -o $@ -lm -lpthread

$(WIN_SONG_TARGET): ct_song_builder.c
	$(WIN_CC) $(CFLAGS) $< -o $@ -lm

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET) $(TARGET).exe

.PHONY: all clean
