# liboriinit — Makefile
#
# Builds a shared library (liboriinit.so) and the oriinit-cli tool.
#
# The library is built as a plain `liboriinit.so` — no version suffix,
# no SONAME machinery. dōgu deploys a single library to a single target;
# the multi-version ABI-coexistence dance that SONAMEs enable isn't
# something we need here.
#
# Build modes — same patterns as tools/dma_listen:
#
#   1. Native on host or target (typically for syntax checking on host,
#      or production build when running directly on the ZCU102):
#        make
#
#   2. Cross-compile via the top-level dōgu Makefile (recommended):
#        cd ..
#        make cross [SYSROOT=...]
#
#   3. Cross-compile directly with SYSROOT variable:
#        make CC=aarch64-linux-gnu-gcc SYSROOT=$HOME/aarch64-sysroot
#
#   4. PetaLinux SDK environment (cleanest if you have the SDK):
#        source /opt/petalinux/.../environment-setup-cortexa72-cortexa53-xilinx-linux
#        make

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -fPIC
LDFLAGS ?=
LDLIBS  := -liio

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
LIBDIR  ?= $(PREFIX)/lib
INCDIR  ?= $(PREFIX)/include

# Vendored libiio v0.25 header
VENDORED_INCLUDE := third_party/libiio-v0.25
CFLAGS  += -Iinclude -I$(VENDORED_INCLUDE)

LIB_SOURCES := src/oriinit.c
LIB_OBJECTS := $(LIB_SOURCES:.c=.o)
LIB_TARGET  := liboriinit.so

CLI_SOURCES := cli/oriinit-cli.c
CLI_TARGET  := oriinit-cli

.PHONY: all clean install help

all: $(LIB_TARGET) $(CLI_TARGET)

# Shared library: plain liboriinit.so, no version suffix.
# The SONAME defaults to the output filename, so the binary will look
# for "liboriinit.so" at runtime. One file, one name.
$(LIB_TARGET): $(LIB_OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -o $@ $^ $(LDLIBS)

# Object compilation (with -fPIC for the shared library)
src/%.o: src/%.c include/oriinit.h
	$(CC) $(CFLAGS) -c $< -o $@

# CLI links against the library we just built. rpath '$ORIGIN' lets it
# find liboriinit.so alongside the binary at runtime (e.g., when both
# are dropped into /home/root/ on the target).
$(CLI_TARGET): $(CLI_SOURCES) $(LIB_TARGET)
	$(CC) $(CFLAGS) $(LDFLAGS) -L. -Wl,-rpath,'$$ORIGIN' \
	      -o $@ $(CLI_SOURCES) -loriinit $(LDLIBS)

clean:
	rm -f $(LIB_TARGET) $(LIB_OBJECTS) $(CLI_TARGET)

install: $(LIB_TARGET) $(CLI_TARGET)
	install -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(BINDIR) $(DESTDIR)$(INCDIR)
	install -m 0644 $(LIB_TARGET) $(DESTDIR)$(LIBDIR)/
	install -m 0755 $(CLI_TARGET) $(DESTDIR)$(BINDIR)/
	install -m 0644 include/oriinit.h $(DESTDIR)$(INCDIR)/

help:
	@echo "liboriinit Makefile targets:"
	@echo "  make          build liboriinit.so + oriinit-cli"
	@echo "  make clean    remove built artifacts"
	@echo "  make install  install to \$$PREFIX (default: /usr/local)"
	@echo ""
	@echo "Cross-compile with SYSROOT variable:"
	@echo "  make CC=aarch64-linux-gnu-gcc SYSROOT=\$$HOME/aarch64-sysroot"
	@echo ""
	@echo "Or use the top-level dōgu Makefile:"
	@echo "  cd .. && make cross"
	@echo ""
	@echo "Or PetaLinux SDK environment:"
	@echo "  source <SDK>/environment-setup-...   then  make"
