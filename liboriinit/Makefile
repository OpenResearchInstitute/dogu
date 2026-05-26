# liboriinit — Makefile
#
# Builds a shared library (liboriinit.so) and the oriinit-cli tool.
#
# Build modes — same patterns as tools/dma_listen:
#
#   1. Native:                              make
#   2. PetaLinux SDK cross-compile:         source <SDK>/environment-setup-...
#                                           make
#   3. Manual aarch64 cross-compile:        make CC=aarch64-linux-gnu-gcc \
#                                                LDFLAGS="-L$HOME/aarch64-sysroot/lib \
#                                                         -Wl,--allow-shlib-undefined"

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -fPIC
LDFLAGS ?=
LDLIBS  := -liio

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
LIBDIR  ?= $(PREFIX)/lib
INCDIR  ?= $(PREFIX)/include

# Vendored libiio v0.25 header
VENDORED_INCLUDE := third_party/libiio-v0.25
CFLAGS  += -Iinclude -I$(VENDORED_INCLUDE)

LIB_SOURCES := src/oriinit.c
LIB_OBJECTS := $(LIB_SOURCES:.c=.o)

LIB_NAME    := liboriinit
LIB_VERSION := 0.1.0
LIB_SONAME  := $(LIB_NAME).so.0
LIB_FULL    := $(LIB_NAME).so.$(LIB_VERSION)
LIB_TARGET  := $(LIB_NAME).so

CLI_SOURCES := cli/oriinit-cli.c
CLI_TARGET  := oriinit-cli

.PHONY: all clean install help

all: $(LIB_TARGET) $(CLI_TARGET)

# Shared library: build with versioned soname; symlinks point to the full
# versioned name.
$(LIB_FULL): $(LIB_OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -Wl,-soname,$(LIB_SONAME) \
	      -o $@ $^ $(LDLIBS)

$(LIB_SONAME): $(LIB_FULL)
	ln -sf $(LIB_FULL) $@

$(LIB_TARGET): $(LIB_SONAME)
	ln -sf $(LIB_SONAME) $@

# Object compilation (with -fPIC for the shared library)
src/%.o: src/%.c include/oriinit.h
	$(CC) $(CFLAGS) -c $< -o $@

# CLI links against the library we just built. Use rpath '.' so it
# finds the library from the build directory during testing; install
# step will write the final rpath as needed.
$(CLI_TARGET): $(CLI_SOURCES) $(LIB_TARGET)
	$(CC) $(CFLAGS) $(LDFLAGS) -L. -Wl,-rpath,'$$ORIGIN' \
	      -o $@ $(CLI_SOURCES) -loriinit $(LDLIBS)

clean:
	rm -f $(LIB_FULL) $(LIB_SONAME) $(LIB_TARGET) $(LIB_OBJECTS) $(CLI_TARGET)

install: $(LIB_TARGET) $(CLI_TARGET)
	install -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(BINDIR) $(DESTDIR)$(INCDIR)
	install -m 0644 $(LIB_FULL) $(DESTDIR)$(LIBDIR)/
	ln -sf $(LIB_FULL) $(DESTDIR)$(LIBDIR)/$(LIB_SONAME)
	ln -sf $(LIB_SONAME) $(DESTDIR)$(LIBDIR)/$(LIB_TARGET)
	install -m 0755 $(CLI_TARGET) $(DESTDIR)$(BINDIR)/
	install -m 0644 include/oriinit.h $(DESTDIR)$(INCDIR)/

help:
	@echo "liboriinit Makefile targets:"
	@echo "  make          build liboriinit.so + oriinit-cli"
	@echo "  make clean    remove built artifacts"
	@echo "  make install  install to \$$PREFIX (default: /usr/local)"
	@echo ""
	@echo "Cross-compile via PetaLinux SDK (recommended):"
	@echo "  source <SDK>/environment-setup-...   then  make"
	@echo ""
	@echo "Manual cross-compile (using copied libiio.so from target):"
	@echo "  make CC=aarch64-linux-gnu-gcc \\"
	@echo "       LDFLAGS=\"-L\$$HOME/aarch64-sysroot/lib -Wl,--allow-shlib-undefined\""
