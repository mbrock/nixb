# Kitty Keyboard Protocol Notes

Source: https://sw.kovidgoyal.net/kitty/keyboard-protocol/

These are implementation notes for `nxt`, not a copy of the spec.

## Quickstart Mode

The first progressive enhancement recommended by the spec is:

```text
CSI > 1 u   push current keyboard mode and enable flag 1
CSI < u     pop one keyboard mode entry on exit
```

Flag `1` is only the first progressive enhancement: disambiguate escape
codes. It is not the full protocol. In this mode:

- Text-producing keys are still delivered as plain UTF-8 bytes.
- Most non-text keys are delivered as `CSI u`, `CSI ~`, or cursor/function
  `CSI` forms.
- Enter, Tab, and Backspace intentionally remain legacy bytes so a user can
  still type recovery commands if an application crashes before restoring the
  mode.
- `Ctrl-C` is delivered as a key event, for example `CSI 99 ; 5 u`, rather
  than as ETX (`0x03`). The terminal driver therefore does not turn it into
  `SIGINT`.

The common forms to parse in quickstart mode are:

```text
CSI number ; modifiers [u~]
CSI 1 ; modifiers [ABCDEFHPQS]
0x0d       Enter
0x7f/0x08  Backspace
0x09       Tab
```

When no modifiers are present, the parameters can be omitted for some
functional keys, yielding forms like `CSI A` for Up.

## Modifiers

Modifiers are encoded as `1 + bitmask`.

```text
shift      1
alt        2
ctrl       4
super      8
hyper      16
meta       32
caps_lock  64
num_lock   128
```

So `Ctrl-C` is `99;5u`: `99` is lowercase `c`, and `5` is `1 + ctrl`.
`Shift-Ctrl-Left` uses modifier value `6`, since `1 + shift + ctrl = 6`.

## Progressive Enhancements

Enhancements are controlled with:

```text
CSI = flags ; mode u
```

`mode` defaults to `1`:

- `1`: set all flags exactly, clearing unspecified flags.
- `2`: set specified flags, leave others alone.
- `3`: reset specified flags, leave others alone.

Flags:

```text
1   disambiguate escape codes
2   report press/repeat/release event types
4   report alternate keys for shortcut matching
8   report all keys as escape codes
16  report associated text with key events
```

The stack forms are:

```text
CSI > flags u   push current flags and apply flags
CSI < number u  pop entries, defaulting to one
```

`nxt::input::InputModeGuard` currently pushes `31` (`1|2|4|8|16`) so the
runtime gets structured key events with event types, alternate key metadata,
and associated text. It still keeps legacy UTF-8/control parsing as fallback
for terminals that ignore the enable sequence.

The spec says main screen and alternate screen have independent keyboard mode
stacks.

## Consequences For nxt

The parser handles both legacy/disambiguation input and the structured mode:

- It should parse `CSI code;mods u`, `CSI number;mods ~`, and
  `CSI 1;mods letter`.
- It still needs legacy fallback for UTF-8 text and control bytes.
- It should treat `Ctrl-C` policy at the application/runtime level, because
  in disambiguation mode it is a normal key event.
- With flag `8` plus `16`, text input arrives as escape-coded key events with
  associated text rather than plain UTF-8 bytes.
- With flag `2`, `KeyEvent` carries `press`, `repeat`, or `release`.
- With flag `4`, `KeyEvent` preserves alternate key fields from the
  colon-separated key-code parameter.

The spec also describes support detection via `CSI ? u` and primary device
attributes. `nxt` currently assumes quickstart support and relies on legacy
fallbacks when a terminal ignores the enable sequence.
