.PHONY: all setup setup-opt build clean docs test vterm-test watch

ifndef IN_NIX_SHELL
$(error You need to run this under 'nix develop')
endif

export CLICOLOR=1

all: setup build
setup:; cmake --preset default
build:; cmake --build --preset default
clean:; cmake --build --preset default --target clean
docs:; doxygen docs/Doxyfile.nix-api
test: build; $(MAKE) -C build test
watch:; find src -name '*.cpp' -o -name '*.hpp' | entr -cs 'make && ./build/nxbcg'
