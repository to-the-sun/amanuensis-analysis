# Makefile for analysis tools

all:
	$(MAKE) -C analysis all

win:
	$(MAKE) -C analysis win

clean:
	$(MAKE) -C analysis clean

.PHONY: all win clean
