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
    RM = del /Q
    RMDIR = rd /s /q
    PYTHON = python
else
    OS_NAME = $(shell uname -s)
    CC = gcc
    CFLAGS = -O3 -Wall -fPIC
    LDFLAGS = -shared
    TARGET = $(TARGET_SO)
    RM = rm -f
    RMDIR = rm -rf
    PYTHON = python3
endif

SRCS = cumulative_transience.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET) extension

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

extension:
	$(PYTHON) setup.py build_ext --inplace

clean:
	$(RM) $(OBJS) $(TARGET_SO) $(TARGET_DLL)
	$(RM) ct_extension.c cumulative_transience.cpython*
	$(RMDIR) build
