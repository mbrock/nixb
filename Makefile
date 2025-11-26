.PHONY: all setup build clean docs test watch

export CLICOLOR_FORCE=1

CMAKE := nix develop -c cmake

all: setup build
setup:; $(CMAKE) --preset default
build:; $(CMAKE) --build --preset default
clean:; $(CMAKE) --build --preset default --target clean
docs:; doxygen docs/Doxyfile.nix-api
watch:; find src -name '*.cpp' -o -name '*.hpp' | entr -cs 'make && ./build/nxbcg'

# Run all tests with nice colorful output
test: build
	./build/default/raster-test || true
	./build/default/terminal-test || true

