# dma_listen — Makefile
#
# Build modes:
#
#   1. Native on target (ZCU102 with libiio runtime):
#        make
#
#   2. Cross-compile (from a Linux host with aarch64 toolchain):
#        make CC=aarch64-linux-gnu-gcc \
#             LDFLAGS="-L$$HOME/aarch64-sysroot/lib -Wl,--allow-shlib-undefined"
#      (assumes ~/aarch64-sysroot/lib/ contains libiio.so copied from
#      the target — see ../../README.md "Cross-compiling" section)
#
#   3. PetaLinux SDK environment (cleanest cross-compile):
#        source /opt/petalinux/2022.2/environment-setup-cortexa72-cortexa53-xilinx-linux
#        make
#      (SDK sets CC and provides a complete sysroot via --sysroot in CFLAGS)

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?=
LDLIBS  := -liio -lm

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

# Always use vendored libiio v0.25 header.
# This protects against:
#   - Build hosts without libiio-dev installed
#   - Build hosts with libiio v1.x installed (deliberately incompatible API)
# See third_party/libiio-v0.25/README.md for rationale.
CFLAGS  += -Ithird_party/libiio-v0.25

TARGET := dma_listen
SOURCES := dma_listen.c

.PHONY: all clean install help

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

clean:
	rm -f $(TARGET) *.o

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

help:
	@echo "dma_listen Makefile targets:"
	@echo "  make            build dma_listen using vendored libiio header"
	@echo "  make clean      remove built artifacts"
	@echo "  make install    install to \$$(BINDIR)  (default: /usr/local/bin)"
	@echo ""
	@echo "Cross-compile with PetaLinux SDK (recommended):"
	@echo "  source <SDK>/environment-setup-...   then  make"
	@echo ""
	@echo "Cross-compile without SDK (using copied libiio.so from target):"
	@echo "  make CC=aarch64-linux-gnu-gcc \\"
	@echo "       LDFLAGS=\"-L\$$HOME/aarch64-sysroot/lib -Wl,--allow-shlib-undefined\""
