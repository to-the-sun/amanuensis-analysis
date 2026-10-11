CC = gcc
WIN_CC = x86_64-w64-mingw32-gcc
CFLAGS = -O3 -I. -Wall -Wextra
LDFLAGS = -lm -lOpenCL -lpthread

TARGET = ct_analyze_headless
WIN_TARGET = ct_analyze_headless.exe
SOURCES = ct_analyze_headless.c ct_exporter.c cumulative_transience.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET) $(WIN_TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@ $(LDFLAGS) || $(CC) $(CFLAGS) $(OBJECTS) -o $@ -lm -lpthread

$(WIN_TARGET): $(SOURCES)
	$(WIN_CC) $(CFLAGS) $(SOURCES) -o $@ -lm -lpthread -lOpenCL || $(WIN_CC) $(CFLAGS) $(SOURCES) -o $@ -lm

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET) $(TARGET).exe

.PHONY: all clean
