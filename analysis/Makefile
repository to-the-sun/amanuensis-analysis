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
    # On Windows, we might not have ldconfig. Assume lfftw3 is in path or provided.
    FFTW_LDFLAGS = -lfftw3
else
    OS_NAME = $(shell uname -s)
    CC = gcc
    CFLAGS = -O3 -Wall -fPIC
    LDFLAGS = -shared
    TARGET = $(TARGET_SO)
    # FFTW library path search for Linux
    FFTW_LIB = $(shell ldconfig -p | grep libfftw3.so.3 | head -n 1 | awk '{print $$4}')
    ifeq ($(FFTW_LIB),)
        FFTW_LDFLAGS = -lfftw3
    else
        FFTW_LDFLAGS = $(FFTW_LIB)
    endif
endif

SRCS = cumulative_transience.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(FFTW_LDFLAGS) -lm

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

mock_external: mock_external.c $(TARGET)
	$(CC) $(CFLAGS) mock_external.c ./$(TARGET) -o $@ -lm

clean:
	rm -f $(OBJS) $(TARGET_SO) $(TARGET_DLL) mock_external mock_external.exe
