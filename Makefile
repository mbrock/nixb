.PHONY: all setup build clean

# Default build dir
BUILDDIR ?= build
MESON ?= meson

all: build

setup:
	$(MESON) setup $(BUILDDIR) --wrap-mode=nodownload

build:
	@if [ ! -f "$(BUILDDIR)/build.ninja" ]; then \
		echo "No build dir; run 'make setup' first" >&2; \
		exit 1; \
	fi
	$(MESON) compile -C $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR) meson-logs meson-private compile_commands.json
