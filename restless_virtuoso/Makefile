WIN_CC = $(shell which x86_64-w64-mingw32-gcc 2>/dev/null || echo /tmp/llvm-mingw-20240619-ucrt-ubuntu-20.04-x86_64/bin/x86_64-w64-mingw32-gcc)
CFLAGS = -O2 -Wall -Wextra -std=c11
LDLIBS = -lwinmm -lole32 -luser32 -ladvapi32 -lshell32

restless_virtuoso.exe: restless_virtuoso.c dsp.c asio_abi.h dsp.h vst_abi.h ../analysis/dr_wav.h
	$(WIN_CC) $(CFLAGS) restless_virtuoso.c dsp.c -o $@ $(LDLIBS)

clean:
	rm -f restless_virtuoso.exe

.PHONY: clean
