# Tests

## Run tests
```bash
make vterm-test                          # boost-ut + vterm tests
./build/src/new/nxb-vterm-tests          # Or run directly
```

## Add a test
Edit `cases/vterm-ut-suite.hpp`:
```cpp
"my test"_test = [] {
  Terminal term(10, 40);
  term.write("\x1b[1mBold\x1b[0m");
  expect(term.get_cell(0, 0)->bold);
};
```

## Test helpers
- `dump_raster(raster, glyphs)` - Visual raster state
- `dump_terminal(term)` - Terminal state inspection
- `capture_output(fn)` - Capture stdout/stderr
- `Raster::count_if(x0, y0, x1, y1, pred)` - Count cells in region
- `Terminal::ScreenSnapshot::count_if(pred)` - Count terminal cells

## boost-ut assertions
- `expect(condition)` - Basic assertion
- `expect(eq(a, b))` - Equality check
- `expect(neq(a, b))` - Inequality check
- `expect(gt(a, b))` - Greater than
- `expect(ge(a, b))` - Greater or equal
- `expect(lt(a, b))` - Less than
- `expect(le(a, b))` - Less or equal
- `expect(!condition)` - Negation
- `expect(condition) << "message"` - Add custom message
