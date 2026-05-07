.PHONY: all setup build run clean watch

export CLICOLOR_FORCE=1

all: build

setup:; meson setup build --wipe 2>/dev/null || meson setup build

build:
	@echo "*** BUILDING"
	meson compile -C build
	@echo "*** FINISHED"

run: build
	./build/nxb

clean:
	rm -rf build

watch:
	find src -name '*.cpp' -o -name '*.hpp' | entr -cs 'make build'
