# dma_listen — Makefile
#
# Build natively on the ZCU102, or cross-compile from a host with the
# aarch64-linux-gnu toolchain.

# Default to native gcc. Override CC for cross-compilation:
#   make CC=aarch64-linux-gnu-gcc
CC      ?= gcc

CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?=
LDLIBS  := -liio -lm

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

TARGET := dma_listen
SOURCES := dma_listen.c
OBJECTS := $(SOURCES:.c=.o)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJECTS)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
