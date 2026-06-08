# dōgu tools/frame_send
# Forwards decoded 134-byte OPV frames (from opv-decode -r) to Interlocutor
# over UDP, one whole frame per datagram, on the OV-frame port (57372).
# Plain C, no libiio — but honors dogu's CC/SYSROOT so the top Makefile's
# `native` and `cross` rules drive it the same as the other components.

CC      ?= cc
CFLAGS  := -O2 -Wall -Wextra
ifneq ($(strip $(SYSROOT)),)
  CFLAGS += --sysroot=$(SYSROOT)
endif

.PHONY: all clean install
all: frame_send
frame_send: frame_send.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f frame_send *.o

install:
	@true
