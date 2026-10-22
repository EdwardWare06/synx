# std.binary

```synx
import std.binary;
import std.binary.{write_uint, read_uint};
```

This module encodes and decodes unsigned integers of 1 to 8 bytes in a byte
vector, in little-endian or big-endian order. Both functions check the width,
the value, and the offset before they access the vector. The checks stay in
release builds. For a signed value, cast it to `ulong` before encoding, and
cast the decoded value back.

## Functions

### write_uint

```synx
bool write_uint(&Vec<byte> bytes, ulong value, usize width, bool little_endian)
```

Appends *value* to *bytes* as *width* bytes. If *little_endian* is `true`,
the least significant byte comes first. Otherwise the most significant byte
comes first.

Returns `true` if the bytes were added. Returns `false` and leaves *bytes*
unchanged if *width* is zero, if *width* is larger than 8, or if *value*
does not fit in *width* bytes.

### read_uint

```synx
ulong? read_uint(&const Vec<byte> bytes, usize offset, usize width, bool little_endian)
```

Reads *width* bytes from *bytes*, starting at index *offset*, as one
unsigned integer. *little_endian* selects the byte order, as for
[write_uint](#write_uint).

Returns `None` if *width* is zero, if *width* is larger than 8, or if the
range from *offset* to *offset* plus *width* is not inside *bytes*. The
bounds check does not overflow for large values of *offset*.

## Example

```synx
import std.binary;

main() {
    Vec<byte> header = Vec.new();
    binary.write_uint(&header, 0x534E, 2, false);
    binary.write_uint(&header, 1000, 4, true);
    print("{header.len()} bytes written");
    print("too large: {binary.write_uint(&header, 300, 1, true)}");

    match binary.read_uint(&header, 0, 2, false) {
        Some(magic) => print("magic {magic}"),
        None => print("short header"),
    }
    match binary.read_uint(&header, 2, 4, true) {
        Some(size) => print("size {size}"),
        None => print("short header"),
    }
    match binary.read_uint(&header, 4, 4, true) {
        Some(_) => print("unexpected"),
        None => print("offset 4 width 4: out of bounds"),
    }
}
```

Output:

```text
6 bytes written
too large: false
magic 21326
size 1000
offset 4 width 4: out of bounds
```
