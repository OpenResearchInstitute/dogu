# dōgu top-level Makefile
# Delegates to each subdirectory's Makefile.

# Subdirectories that have something to build right now.
# Add to this list as new components come online.
SUBDIRS := tools/dma_listen liboriinit

# -----------------------------------------------------------------------
# Cross-compile defaults — used by `make cross`.
# Override on the command line if needed:
#
#   make cross SYSROOT=/opt/petalinux/.../sysroots/cortexa72-cortexa53-xilinx-linux
#   make cross CROSS_COMPILE=aarch64-poky-linux-
# -----------------------------------------------------------------------

# Path to the aarch64 sysroot containing libiio runtime + headers.
SYSROOT          ?= $(HOME)/aarch64-sysroot

# Toolchain prefix. The compiler used will be $(CROSS_COMPILE)gcc.
CROSS_COMPILE    ?= aarch64-linux-gnu-

# -----------------------------------------------------------------------
# Deploy defaults — used by `make deploy`.
# Override on the command line if needed:
#
#   make deploy DEPLOY_HOST=root@haifuraiya.local
#   make deploy DEPLOY_PATH=/opt/ori/dogu
# -----------------------------------------------------------------------
DEPLOY_HOST      ?= root@10.73.1.16
DEPLOY_PATH      ?= /home/root

.PHONY: all native cross deploy clean install help $(SUBDIRS)

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
	@echo "==> Deploying to $(DEPLOY_HOST):$(DEPLOY_PATH)"
	@echo "==> Removing any stale liboriinit.so* files on target"
	ssh $(DEPLOY_HOST) 'rm -f $(DEPLOY_PATH)/liboriinit.so*'
	@echo "==> Copying binaries + library"
	scp tools/dma_listen/dma_listen   $(DEPLOY_HOST):$(DEPLOY_PATH)/dma_listen
	scp liboriinit/oriinit-cli         $(DEPLOY_HOST):$(DEPLOY_PATH)/oriinit-cli
	scp liboriinit/liboriinit.so       $(DEPLOY_HOST):$(DEPLOY_PATH)/liboriinit.so
	@echo ""
	@echo "Deploy complete. Verify on the target:"
	@echo "  ssh $(DEPLOY_HOST) 'ls -la $(DEPLOY_PATH)/{dma_listen,oriinit-cli,liboriinit.so}'"
	@echo "  ssh $(DEPLOY_HOST) '$(DEPLOY_PATH)/oriinit-cli status'"

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
	@echo "                     CROSS_COMPILE = aarch64-linux-gnu-"
	@echo "                     SYSROOT       = \$$HOME/aarch64-sysroot"
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
