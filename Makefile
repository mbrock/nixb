.PHONY: all setup build clean docs test

ifndef IN_NIX_SHELL
$(error You need to run this under 'nix develop')
endif

# Default build dir
BUILDDIR ?= build
MESON ?= meson
SETUP_FLAGS ?= --wrap-mode=nodownload

all: build

build: $(BUILDDIR)/build.ninja
	$(MESON) compile -C $(BUILDDIR)

setup: $(BUILDDIR)/build.ninja

$(BUILDDIR)/build.ninja:
	$(MESON) setup $(BUILDDIR) $(SETUP_FLAGS)

clean:
	rm -rf $(BUILDDIR) meson-logs meson-private compile_commands.json

docs:; doxygen docs/Doxyfile.nix-api

test: build
	./test.sh
