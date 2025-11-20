.PHONY: all setup build clean

# Default build dir
BUILDDIR ?= build
MESON ?= meson

all: build

setup:
	$(MESON) setup $(BUILDDIR) --wrap-mode=nodownload

build:
	$(MESON) compile -C $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR) meson-logs meson-private compile_commands.json
