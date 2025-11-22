.PHONY: all setup build build-opt clean docs test vterm-test

ifndef IN_NIX_SHELL
$(error You need to run this under 'nix develop')
endif

# Default build dir
BUILDDIR ?= build
MESON ?= meson
SETUP_FLAGS ?= --wrap-mode=nodownload

all: build

build: $(BUILDDIR)/build.ninja
	$(MESON) compile -C $(BUILDDIR) -j$$(nproc) -v

setup: $(BUILDDIR)/build.ninja

$(BUILDDIR)/build.ninja:
	$(MESON) setup $(BUILDDIR) $(SETUP_FLAGS)

build-opt: $(BUILDDIR)/build.ninja
	$(MESON) setup $(BUILDDIR) $(SETUP_FLAGS) --buildtype=release -Doptimization=3 -Db_ndebug=true --reconfigure
	$(MESON) compile -C $(BUILDDIR) -j$$(nproc) -v

clean:
	rm -rf $(BUILDDIR) meson-logs meson-private compile_commands.json

docs:; doxygen docs/Doxyfile.nix-api

test: build
	./test.sh

# Modern vterm-based C++ tests using doctest
vterm-test: build
	reset; clear;
	$(BUILDDIR)/src/new/nxb-vterm-tests

# Run all tests including legacy test runner
test-all: test vterm-test
	$(BUILDDIR)/src/new/nxb-test-runner basic_ansi
	$(BUILDDIR)/src/new/nxb-test-runner compositor_basic
