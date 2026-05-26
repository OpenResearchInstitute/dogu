# dōgu top-level Makefile
# Delegates to each subdirectory's Makefile.

# Subdirectories that have something to build right now.
# Add to this list as new components come online.
SUBDIRS := tools/dma_listen liboriinit

.PHONY: all clean install $(SUBDIRS)

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

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
	@echo "  all      build all subdirectories ($(SUBDIRS))"
	@echo "  clean    clean all subdirectories"
	@echo "  install  install all subdirectories' artifacts"
	@echo "  help     this message"
	@echo ""
	@echo "Build single component:"
	@echo "  cd tools/dma_listen && make"
