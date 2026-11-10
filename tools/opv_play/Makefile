# dōgu tools/opv_play
# VCK190 memory-loopback player/capturer: registers via /dev/uio0, stimulus
# through /dev/dma_proxy_tx, soft bits from /dev/dma_proxy_rx.  Plain C, no
# libiio -- honors dogu's CC/SYSROOT like frame_send so the top Makefile's
# `native` and `cross` rules drive it the same as the other components.
# dma-proxy.h is the copy vendored in the MDT repo's PetaLinux recipe
# (BUFFER_SIZE 4 MB) and must match the module on the board.

CC      ?= cc
CFLAGS  := -O2 -Wall -Wextra -std=gnu11
ifneq ($(strip $(SYSROOT)),)
  CFLAGS += --sysroot=$(SYSROOT)
endif

.PHONY: all clean install
all: opv_play
opv_play: opv_play.c dma-proxy.h
	$(CC) $(CFLAGS) -o $@ opv_play.c

clean:
	rm -f opv_play *.o

install:
	@true
