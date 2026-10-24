# opv_play -- VCK190 memory-loopback player/capturer.  Cross-built for the
# A72s like dogu's other tools; dma-proxy.h is the copy vendored in the MDT
# repo's PetaLinux recipe (BUFFER_SIZE 4 MB) and must match the module.
CROSS_COMPILE ?= aarch64-linux-gnu-
CC      := $(CROSS_COMPILE)gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
TARGET  := opv_play

all: $(TARGET)

$(TARGET): opv_play.c dma-proxy.h
	$(CC) $(CFLAGS) -o $@ opv_play.c

# host build for a syntax/semantics check only (no /dev/uio0 here)
host: CROSS_COMPILE =
host: $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all host clean
