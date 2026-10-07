# Makefile for pitch_separator

CC ?= gcc
CFLAGS ?= -O3 -Wall -std=c99
LDFLAGS ?= -lm

TARGET = pitch_separator
SRCS = pitch_separator.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) $(TARGET).exe

.PHONY: all clean
