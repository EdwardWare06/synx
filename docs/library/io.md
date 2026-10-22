# std.io

```synx
import std.io;
import std.io.{print, read_line, input};
```

This module reads lines from standard input and re-exports `print` from the
prelude. Input is read as bytes and decoded as UTF-8. For files, see
[std.fs](fs.md).

The error type `IoError` and its `ErrorKind` are defined in
[std.fs](fs.md#ioerror) and re-exported here.

## Functions

### print

```synx
void print(&const str text)
```

Writes *text* and a line feed to standard output. The write is done as one
operation: lines printed by different threads do not mix. `print` is in the
prelude, so it needs no import.

A `String` argument is borrowed automatically. An interpolated literal such
as `"total: {n}"` is a `String`.

### read_line

```synx
Result<String?, IoError> read_line()
```

Reads one line from standard input.

Returns `Ok(Some(line))` for a line, `Ok(Some(""))` for a blank line, and
`Ok(None)` when the input ends before any byte is read. The line feed is
removed. A carriage return immediately before the line feed is also removed.
A carriage return at the end of the input, with no line feed after it, is
kept.

Errors:

- `ErrorKind.InvalidUtf8` if the line is not valid UTF-8.
- `ErrorKind.Other`, with the operating system code in `os_code`, if the
  read fails.

Standard input is locked for the duration of the call. Two threads that call
`read_line` at the same time each receive complete lines.

### input

```synx
String input()
```

Reads one line from standard input, like [read_line](#read_line), and returns
it. A blank line and the end of input both return an empty string. A loop that
reads until the end of input must stop on an empty result, or use
`read_line` to tell the two cases apart.

Panics with `cannot read UTF-8 input` if the read fails or the line is not
valid UTF-8.

## Example

Count the lines and bytes of standard input:

```synx
import std.io.read_line;

int main() {
    usize lines = 0;
    usize bytes = 0;
    loop {
        match read_line() {
            Ok(Some(line)) => { lines += 1; bytes += line.len(); },
            Ok(None) => break,
            Err(_) => { print("input is not UTF-8"); return 1; },
        }
    }
    print("{lines} lines, {bytes} bytes");
    return 0;
}
```

For the input `one`, `two`, `three` on three lines, the output is:

```text
3 lines, 11 bytes
```
