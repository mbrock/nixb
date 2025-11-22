.PHONY: all setup setup-opt build clean docs test vterm-test

ifndef IN_NIX_SHELL
$(error You need to run this under 'nix develop')
endif

MESON ?= meson
SETUP_FLAGS ?= --wrap-mode=nodownload

all: build
build: build/build.ninja; ninja -C build

build/build.ninja: meson.build
	$(MESON) setup build $(SETUP_FLAGS)

setup: build/build.ninja

setup-opt: build/meson.build
	$(MESON) setup build $(SETUP_FLAGS) \
		--buildtype=release \
		-Doptimization=3 \
		-Db_ndebug=true \
		--reconfigure

clean:; rm -rf build

docs:; doxygen docs/Doxyfile.nix-api

test: build vterm-test
	./test.sh

vterm-test: build
	./build/nxb-vterm-tests
