# dōgu top-level Makefile
# Delegates to each subdirectory's Makefile.

# Subdirectories that have something to build right now.
# Add to this list as new components come online.
SUBDIRS := tools/dma_listen liboriinit frame_decoder
SUBDIRS := tools/dma_listen liboriinit frame_decoder tools/frame_send

# -----------------------------------------------------------------------
# Cross-compile defaults — used by `make cross`.
# Override on the command line if needed:
#
#   make cross SYSROOT=/path/to/aarch64/sysroot
#   make cross XILINX_GNU_DIR=/opt/Xilinx/Vitis/2023.2/.../bin
#   make cross CROSS_COMPILE=/some/other/toolchain/aarch64-linux-gnu-
# -----------------------------------------------------------------------

# Path to the aarch64 sysroot containing libiio runtime + headers.
SYSROOT          ?= $(HOME)/petalinux-sdk/sysroots/cortexa72-cortexa53-xilinx-linux

# Cross toolchain.
#
# We deliberately use the Xilinx/Vitis-bundled aarch64 Linux GNU toolchain,
# NOT Ubuntu's gcc-aarch64-linux-gnu apt package. Reason: that apt package
# conflicts with gcc-multilib (which PetaLinux requires), so installing one
# evicts the other — every switch between "build PetaLinux" and "build dogu"
# silently uninstalled the toolchain we needed. The Vitis toolchain lives
# outside apt, so the two coexist permanently. It is also GCC 11.2 / glibc,
# ABI-matched to the PetaLinux 2022.2 target rootfs.
#
# XILINX_GNU_DIR points at the directory holding aarch64-linux-gnu-gcc.
# Override it if your Xilinx install path or version differs.
XILINX_GNU_DIR   ?= /opt/Xilinx/Vitis/2022.2/gnu/aarch64/lin/aarch64-linux/bin

# Toolchain prefix. The compiler used will be $(CROSS_COMPILE)gcc.
CROSS_COMPILE    ?= $(XILINX_GNU_DIR)/aarch64-linux-gnu-

# -----------------------------------------------------------------------
# Deploy defaults — used by `make deploy`.
# Override on the command line if needed:
#
#   make deploy DEPLOY_HOST=root@haifuraiya.local
#   make deploy DEPLOY_PATH=/opt/ori/dogu
# -----------------------------------------------------------------------
DEPLOY_HOST      ?= root@10.73.1.16
DEPLOY_PATH      ?= /home/root

# -----------------------------------------------------------------------
# Self-test defaults — used by `make test-frame-decoder`.
# Generates a known soft-symbol fixture from the repo's own host tools,
# decodes it both on the host (golden) and on the target, and asserts the
# two agree. Override frame count / station as needed.
# -----------------------------------------------------------------------
TEST_FRAMES   ?= 20
TEST_STATION  ?= W5NYV

.PHONY: all native cross deploy clean install help test test-frame-decoder $(SUBDIRS)

# Default: native build for host (matches pre-existing behavior).
all: native

native: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

# Cross-compile build for the aarch64 target (ZCU102 deployment).
# Each subdir respects the SYSROOT variable; see Makefile.dma_listen
# and Makefile.liboriinit for how it's wired up.
cross:
	@for d in $(SUBDIRS); do \
		echo "==> cross-compiling $$d"; \
		$(MAKE) -C $$d \
			CC="$(CROSS_COMPILE)gcc" \
			SYSROOT="$(SYSROOT)" \
		|| exit 1; \
	done
	@echo ""
	@echo "Cross-compile complete. Verify aarch64:"
	@echo "  file tools/dma_listen/dma_listen liboriinit/oriinit-cli liboriinit/liboriinit.so"

# Deploy cross-compiled binaries + library to a target board over scp.
# Cleans up old versioned .so symlinks/copies first to avoid the
# dynamic linker picking up stale code from previous deployments.
#
# Run `make cross` first (deploy doesn't trigger a rebuild — keeps
# the workflow explicit about what's being shipped).
deploy:
	@if [ ! -x liboriinit/oriinit-cli ] || [ ! -f liboriinit/liboriinit.so ]; then \
		echo "ERROR: liboriinit artifacts missing — run 'make cross' first." >&2; \
		exit 1; \
	fi
	@if [ ! -x tools/dma_listen/dma_listen ]; then \
		echo "ERROR: dma_listen missing — run 'make cross' first." >&2; \
		exit 1; \
	fi
	@if [ ! -x frame_decoder/opv-decode ]; then \
		echo "ERROR: frame_decoder/opv-decode missing — run 'make cross' first." >&2; \
		exit 1; \
	fi
	@echo "==> Deploying to $(DEPLOY_HOST):$(DEPLOY_PATH)"
	@echo "==> Removing any stale liboriinit.so* files on target"
	ssh $(DEPLOY_HOST) 'rm -f $(DEPLOY_PATH)/liboriinit.so*'
	@echo "==> Copying binaries + library"
	scp tools/dma_listen/dma_listen   $(DEPLOY_HOST):$(DEPLOY_PATH)/dma_listen
	scp tools/frame_send/frame_send    $(DEPLOY_HOST):$(DEPLOY_PATH)/frame_send
	scp liboriinit/oriinit-cli         $(DEPLOY_HOST):$(DEPLOY_PATH)/oriinit-cli
	scp liboriinit/liboriinit.so       $(DEPLOY_HOST):$(DEPLOY_PATH)/liboriinit.so
	@file frame_decoder/opv-decode | grep -q 'aarch64' || \
	{ echo "ERROR: opv-decode is not aarch64 — run 'make cross' (host build clobbered it)"; exit 1; }
	scp frame_decoder/opv-decode       $(DEPLOY_HOST):$(DEPLOY_PATH)/opv-decode
	scp bring-up.sh                    $(DEPLOY_HOST):$(DEPLOY_PATH)/bring-up.sh
	@if [ -d profiles ]; then \
		echo "==> Copying TES profiles"; \
		for f in profiles/*.bin profiles/*.json; do \
			[ -f "$$f" ] && scp "$$f" $(DEPLOY_HOST):$(DEPLOY_PATH)/; \
		done; \
	fi
	ssh $(DEPLOY_HOST) 'chmod +x $(DEPLOY_PATH)/bring-up.sh'
	@echo ""
	@echo "Deploy complete. Verify on the target:"
	@echo "  ssh $(DEPLOY_HOST) 'ls -la $(DEPLOY_PATH)/{dma_listen,oriinit-cli,liboriinit.so}'"
	@echo "  ssh $(DEPLOY_HOST) '$(DEPLOY_PATH)/oriinit-cli status'"
	@echo "  ssh $(DEPLOY_HOST) '$(DEPLOY_PATH)/opv-decode -q -r < test_soft.s16 | wc -c'   # 134 bytes/frame"
	@echo "  make test-frame-decoder      # end-to-end: host-golden vs on-target decode"
	@echo "  bring-up.sh tes_0231_Haifuraiya_FDD_LVDS_20Msps_10MHz"

# -----------------------------------------------------------------------
# Self-test: prove the deployed opv-decode runs on the A53 and produces the
# exact same frames as the host reference build.
#   1. build host opv-mod / opv-demod / opv-decode from the submodule
#   2. mod TEST_FRAMES frames -> demod (-X taps the int16 soft stream the
#      fabric will emit) -> host opv-decode = golden frames
#   3. ship the soft fixture to the target, decode with the deployed aarch64
#      opv-decode, pull the frames back
#   4. assert: target frame count == TEST_FRAMES AND target == host golden
# Requires `make cross && make deploy` first. The soft fixture is the same
# bytes both sides decode, so a correct cross-arch decode is bit-identical.
# -----------------------------------------------------------------------
test: test-frame-decoder

test-frame-decoder:
	@echo "==> Building host reference tools from the submodule"
	@$(MAKE) -s -C frame_decoder/opv-cxx-demod bin/opv-mod bin/opv-demod bin/opv-decode >/dev/null
	@OPV=frame_decoder/opv-cxx-demod; \
	 echo "==> Generating $(TEST_FRAMES)-frame soft fixture (station $(TEST_STATION))"; \
	 $$OPV/bin/opv-mod -S $(TEST_STATION) -B $(TEST_FRAMES) 2>/dev/null \
	   | $$OPV/bin/opv-demod -s -c -q -X /tmp/dogu_fd_soft.s16 >/dev/null 2>&1; \
	 $$OPV/bin/opv-decode -q -r < /tmp/dogu_fd_soft.s16 > /tmp/dogu_fd_golden.bin 2>/dev/null; \
	 GF=$$(( $$(wc -c < /tmp/dogu_fd_golden.bin) / 134 )); \
	 echo "    host golden: $$GF frames"; \
	 echo "==> Shipping fixture to $(DEPLOY_HOST) and decoding on target"; \
	 scp -q /tmp/dogu_fd_soft.s16 $(DEPLOY_HOST):$(DEPLOY_PATH)/test_soft.s16; \
	 ssh $(DEPLOY_HOST) '$(DEPLOY_PATH)/opv-decode -q -r < $(DEPLOY_PATH)/test_soft.s16 > $(DEPLOY_PATH)/test_frames.bin 2>/dev/null'; \
	 scp -q $(DEPLOY_HOST):$(DEPLOY_PATH)/test_frames.bin /tmp/dogu_fd_target.bin; \
	 TF=$$(( $$(wc -c < /tmp/dogu_fd_target.bin) / 134 )); \
	 echo "    target:      $$TF frames"; \
	 if cmp -s /tmp/dogu_fd_golden.bin /tmp/dogu_fd_target.bin && [ "$$TF" -eq "$(TEST_FRAMES)" ]; then \
	   echo "✓ frame_decoder: A53 decode is bit-identical to host reference ($$TF/$(TEST_FRAMES) frames)"; \
	 else \
	   echo "✗ frame_decoder: mismatch (host $$GF, target $$TF) — investigate"; exit 1; \
	 fi

clean:
	@for d in $(SUBDIRS); do \
		$(MAKE) -C $$d clean; \
	done

install:
	@for d in $(SUBDIRS); do \
		$(MAKE) -C $$d install; \
	done

help:
	@echo "dōgu — ORI ARM-side tooling for ADRV9001/9002 SDR builds"
	@echo ""
	@echo "Targets:"
	@echo "  all / native     build all subdirectories for the host (default)"
	@echo "                   useful for syntax checking and local development"
	@echo ""
	@echo "  cross            cross-compile all subdirectories for aarch64"
	@echo "                   uses:"
	@echo "                     CC            = \$$(CROSS_COMPILE)gcc"
	@echo "                     SYSROOT       = \$$(SYSROOT)"
	@echo "                   defaults:"
	@echo "                     XILINX_GNU_DIR = /opt/Xilinx/Vitis/2022.2/gnu/aarch64/lin/aarch64-linux/bin"
	@echo "                     CROSS_COMPILE  = \$$(XILINX_GNU_DIR)/aarch64-linux-gnu-"
	@echo "                     SYSROOT        = \$$HOME/aarch64-sysroot"
	@echo "                   (Vitis-bundled toolchain, not Ubuntu's apt cross-gcc —"
	@echo "                    the apt one conflicts with gcc-multilib / PetaLinux)"
	@echo ""
	@echo "  deploy           ship cross-compiled binaries to the target board"
	@echo "                   removes stale liboriinit.so* first to avoid the"
	@echo "                   dynamic linker picking up old code"
	@echo "                   uses:"
	@echo "                     DEPLOY_HOST   = \$$(DEPLOY_HOST)"
	@echo "                     DEPLOY_PATH   = \$$(DEPLOY_PATH)"
	@echo "                   defaults:"
	@echo "                     DEPLOY_HOST   = root@10.73.1.16"
	@echo "                     DEPLOY_PATH   = /home/root"
	@echo ""
	@echo "  clean            clean all subdirectories"
	@echo "  install          install all subdirectories' artifacts"
	@echo "  help             this message"
	@echo ""
	@echo "Common invocations:"
	@echo "  make                          # native build for host"
	@echo "  make clean && make cross      # cross-compile for ZCU102 (aarch64)"
	@echo "  make deploy                   # ship to the ZCU102"
	@echo "  make clean && make cross && make deploy   # full rebuild + ship"
	@echo ""
	@echo "Subdirectories: $(SUBDIRS)"
