.PHONY: all setup build clean watch

export CLICOLOR_FORCE=1

all: setup build

setup:
	meson setup build --wipe 2>/dev/null || meson setup build

build:
	meson compile -C build

clean:
	rm -rf build

watch:
	find src -name '*.cpp' -o -name '*.hpp' | entr -cs 'make build'
