# Makefile for libtransience

# Default targets
TARGET_SO = libtransience.so
TARGET_DLL = libtransience.dll

# Detect OS
ifeq ($(OS),Windows_NT)
    OS_NAME = Windows
    CC = gcc
    CFLAGS = -O3 -Wall -fPIC
    LDFLAGS = -shared
    TARGET = $(TARGET_DLL)
else
    OS_NAME = $(shell uname -s)
    CC = gcc
    CFLAGS = -O3 -Wall -fPIC
    LDFLAGS = -shared
    TARGET = $(TARGET_SO)
endif

SRCS = cumulative_transience.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

mock_external: mock_external.c $(TARGET)
	$(CC) $(CFLAGS) mock_external.c ./$(TARGET) -o $@ -lm

clean:
	rm -f $(OBJS) $(TARGET_SO) $(TARGET_DLL) mock_external mock_external.exe
