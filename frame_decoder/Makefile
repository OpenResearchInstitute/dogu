# dōgu frame_decoder component
# ---------------------------------------------------------------------------
# Wraps the opv-cxx-demod submodule and builds the A53 decode-only binary,
# `opv-decode` — the fabric-demod-split frame decoder.
#
# Pipeline (where this fits):
#   PL fabric:  channelizer -> MSK demod -> frame sync detector (soft bits)
#               [hard real time, in MDT / msk_demodulator / pluto_msk]
#   --- AXI-Stream frame boundary: first stage with NO hard real-time req ---
#   A53 (here): opv-decode -> sync-resolve + deinterleave + Viterbi +
#               derandomize -> decoded 134-byte OPV frames -> Interlocutor
#
# opv-decode is dependency-light (libstdc++/libm/libc only — no libiio, no
# Xilinx/ADI deps), so unlike dma_listen and liboriinit it needs no libiio
# sysroot. We therefore do NOT forward dogu's libiio SYSROOT into this C++
# build; the cross toolchain's own sysroot supplies libc/libstdc++.
#
# How dogu drives this: the top Makefile sets CC on the command line —
# the built-in "cc" for `native`, $(CROSS_COMPILE)gcc for `cross`. A plain
# `CC ?= gcc` would NOT override Make's built-in CC=cc, so we detect the
# cross toolchain by its aarch64 prefix and derive the C++ compiler only in
# that case; for host we let the submodule use its own `CXX ?= g++` default.
# Command-line CXX=… beats the submodule's built-in assignment, as the
# opv-cxx-demod README documents.
# ---------------------------------------------------------------------------

SUBMODULE  := opv-cxx-demod
DEPLOY_BIN := opv-decode          # the single binary dogu ships to the A53

ifneq (,$(findstring aarch64,$(CC)))
  # Cross: dogu passed CC=<prefix>aarch64-...-gcc on the command line.
  OPV_BIN      := bin/a53/$(DEPLOY_BIN)
  SUBMAKE_ARGS := TARGET=a53 CXX=$(CC:gcc=g++)
else
  # Host/native: let the submodule pick its own g++.
  OPV_BIN      := bin/$(DEPLOY_BIN)
  SUBMAKE_ARGS := TARGET=host
endif

.PHONY: all native cross clean install
all native:
	$(MAKE) -C $(SUBMODULE) $(SUBMAKE_ARGS) $(OPV_BIN)
	cp -f $(SUBMODULE)/$(OPV_BIN) ./$(DEPLOY_BIN)
	@echo "frame_decoder: built $(DEPLOY_BIN) [$(SUBMAKE_ARGS)]"

# Standalone cross entry point (the submodule's a53 branch defaults
# CXX=aarch64-linux-gnu-g++; override on the command line if yours differs).
cross:
	$(MAKE) -C $(SUBMODULE) TARGET=a53 bin/a53/$(DEPLOY_BIN)
	cp -f $(SUBMODULE)/bin/a53/$(DEPLOY_BIN) ./$(DEPLOY_BIN)
	@echo "frame_decoder: cross-built $(DEPLOY_BIN) (aarch64)"

clean:
	-$(MAKE) -C $(SUBMODULE) clean
	rm -f ./$(DEPLOY_BIN)

install:
	@true
