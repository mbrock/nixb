# Tests

## Run tests
```bash
make vterm-test                          # Doctest+vterm tests
./build/src/new/nxb-vterm-tests          # Or run directly
```

## Add a test
Edit `cases/vterm-doctest-suite.hpp`:
```cpp
TEST_CASE("my test") {
  Terminal term(10, 40);
  term.write("\x1b[1mBold\x1b[0m");
  CHECK(term.get_cell(0, 0)->bold);
}
```

## Test helpers
- `dump_raster(raster, glyphs)` - Visual raster state
- `dump_terminal(term)` - Terminal state inspection
- `capture_output(fn)` - Capture stdout/stderr
- `Raster::count_if(x0, y0, x1, y1, pred)` - Count cells in region
- `Terminal::ScreenSnapshot::count_if(pred)` - Count terminal cells
