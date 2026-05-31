# dma_listen — Makefile
#
# Build modes:
#
#   1. Native on host or target (typically for syntax checking on host,
#      or production build when running directly on the ZCU102):
#        make
#
#   2. Cross-compile via the top-level dōgu Makefile (recommended):
#        cd ../..
#        make cross [SYSROOT=...]
#
#   3. Cross-compile directly with SYSROOT variable:
#        make CC=aarch64-linux-gnu-gcc SYSROOT=$HOME/aarch64-sysroot
#
#   4. PetaLinux SDK environment (cleanest if you have the SDK):
#        source /opt/petalinux/.../environment-setup-cortexa72-cortexa53-xilinx-linux
#        make
#      (SDK sets CC and provides --sysroot via CFLAGS automatically)

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?=
LDLIBS  := -liio -lm

# -----------------------------------------------------------------------
# Optional SYSROOT for manual cross-compile (Build Mode #3).
# When set, adds -L $(SYSROOT)/lib and --allow-shlib-undefined so the
# linker can resolve libiio.so even if it has dependencies not present
# in the sysroot.
# -----------------------------------------------------------------------
SYSROOT ?=
ifneq ($(strip $(SYSROOT)),)
  CFLAGS  += --sysroot=$(SYSROOT)
  LDFLAGS += --sysroot=$(SYSROOT) -L$(SYSROOT)/usr/lib -L$(SYSROOT)/lib -Wl,--allow-shlib-undefined
endif

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

# Always use the vendored libiio v0.25 header.
# Protects against:
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
	@echo "  make install    install to \$$BINDIR (default: /usr/local/bin)"
	@echo ""
	@echo "Cross-compile with SYSROOT variable:"
	@echo "  make CC=aarch64-linux-gnu-gcc SYSROOT=\$$HOME/aarch64-sysroot"
	@echo ""
	@echo "Or use the top-level dōgu Makefile:"
	@echo "  cd ../.. && make cross"
	@echo ""
	@echo "Or PetaLinux SDK environment:"
	@echo "  source <SDK>/environment-setup-...   then  make"
