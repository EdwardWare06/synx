# std.utf8

```synx
import std.utf8;
import std.utf8.{encode, decode};
```

This module converts between text and byte vectors and checks that bytes are
valid UTF-8. For operations on text, see [std.text](text.md).

## Functions

### decode

```synx
String? decode(&const Vec<byte> bytes)
```

Checks that *bytes* is valid UTF-8 and returns a new `String` with a copy of
the bytes. An empty vector gives an empty string.

Returns `None` if *bytes* contains any of these:

- a byte that cannot start a sequence (`0x80` to `0xC1`, or `0xF5` to
  `0xFF`)
- a sequence that ends before all of its continuation bytes
- a continuation byte outside `0x80` to `0xBF`
- an overlong encoding
- a surrogate code point, U+D800 to U+DFFF
- a value above U+10FFFF

### encode

```synx
Vec<byte> encode(&const str text)
```

Returns a new vector with a copy of the UTF-8 bytes of *text*. The length of
the result is `text.len()`.

## Example

```synx
import std.utf8;

main() {
    var bytes = utf8.encode("caf\u{e9}");
    print("{bytes.len()} bytes");
    match utf8.decode(&bytes) {
        Some(text) => print("decoded: {text}"),
        None => print("invalid"),
    }
    bytes.pop();
    match utf8.decode(&bytes) {
        Some(text) => print("decoded: {text}"),
        None => print("truncated sequence: invalid"),
    }
}
```

Output:

```text
5 bytes
decoded: café
truncated sequence: invalid
```
