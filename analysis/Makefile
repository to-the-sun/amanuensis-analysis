CC = gcc
CFLAGS = -O3 -Wall -fPIC
LDFLAGS = -shared
TARGET = libtransience.so

# FFTW library path search
FFTW_LIB = $(shell ldconfig -p | grep libfftw3.so.3 | head -n 1 | awk '{print $$4}')
ifeq ($(FFTW_LIB),)
    FFTW_LDFLAGS = -lfftw3
else
    FFTW_LDFLAGS = $(FFTW_LIB)
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
	rm -f $(OBJS) $(TARGET) mock_external
