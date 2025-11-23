.PHONY: all setup setup-opt build clean docs test vterm-test

ifndef IN_NIX_SHELL
$(error You need to run this under 'nix develop')
endif

CMAKE ?= cmake
BUILD_TYPE ?= Debug

export CLICOLOR=1

all: build
build: build/Makefile; make -C build -j`nproc`
build/Makefile: setup
setup: CMakeLists.txt; $(CMAKE) --preset default
clean:; rm -rf build
docs:; doxygen docs/Doxyfile.nix-api
test: build; $(MAKE) -C build test
