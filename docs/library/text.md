# std.text

```synx
import std.text;
import std.text.{find, replace, lines, parse_long};
```

This module searches, slices, splits, and converts UTF-8 text. The functions
borrow their input and return owned results. Offsets and lengths count
bytes. Only [code_points](#code_points) and
[from_code_point](#from_code_point) work with Unicode scalar values. For
byte vectors, see [std.utf8](utf8.md).

## Functions

### is_boundary

```synx
bool is_boundary(&const str text, usize index)
```

Returns `true` if *index* is the start of a UTF-8 sequence in *text*, or is
zero, or is `text.len()`. Returns `false` if *index* is inside a multi-byte
sequence or larger than `text.len()`.

### slice

```synx
String? slice(&const str text, usize start, usize end)
```

Returns a copy of the bytes of *text* from index *start* to index *end*.
The byte at *end* is excluded. Returns `None` if *start* is larger than *end*, or if *start* or
*end* fails [is_boundary](#is_boundary).

### find

```synx
usize? find(&const str text, &const str needle)
```

Returns the byte offset of the first occurrence of *needle* in *text*.
Returns `None` if there is no occurrence. An empty *needle* gives
`Some(0)`.

### rfind

```synx
usize? rfind(&const str text, &const str needle)
```

Returns the byte offset of the last occurrence of *needle* in *text*.
Returns `None` if there is no occurrence. An empty *needle* gives
`Some(text.len())`.

### starts_with

```synx
bool starts_with(&const str text, &const str prefix)
```

Returns `true` if *text* starts with *prefix*. An empty *prefix* gives
`true`.

### ends_with

```synx
bool ends_with(&const str text, &const str suffix)
```

Returns `true` if *text* ends with *suffix*. An empty *suffix* gives `true`.

### replace

```synx
String replace(&const str text, &const str needle, &const str replacement)
```

Returns a copy of *text* in which each occurrence of *needle* is replaced
with *replacement*. The search goes from left to right and continues after
each replaced occurrence, so occurrences do not overlap. For example,
replacing `aa` with `b` in `aaaa` gives `bb`.

Panics with `replace requires a nonempty search string` if *needle* is
empty.

### to_ascii_lowercase

```synx
String to_ascii_lowercase(&const str text)
```

Returns a copy of *text* in which the ASCII letters `A` to `Z` are changed
to `a` to `z`. All other characters stay the same.

### to_ascii_uppercase

```synx
String to_ascii_uppercase(&const str text)
```

Returns a copy of *text* in which the ASCII letters `a` to `z` are changed
to `A` to `Z`. All other characters stay the same.

### join

```synx
String join(&const Vec<String> parts, &const str separator)
```

Returns the strings in *parts*, in order, with *separator* between each
pair. An empty vector gives an empty string.

### lines

```synx
Vec<String> lines(&const str text)
```

Splits *text* at each line feed (LF) and returns the parts. The line feeds
are removed. A carriage return (CR) immediately before a line feed is also
removed. A CR in other positions stays in the text.

Empty lines are included. Text that ends with a line feed gives an empty
last part. An empty *text* gives one empty part. For example,
`"a\r\nb\n\nc"` gives `a`, `b`, an empty string, and `c`.

### code_points

```synx
Vec<uint> code_points(&const str text)
```

Returns the Unicode scalar values of *text*, in order.

### from_code_point

```synx
String? from_code_point(uint point)
```

Returns a string that holds the one character with the scalar value
*point*. Returns `None` if *point* is a surrogate, U+D800 to U+DFFF, or is
larger than U+10FFFF.

### parse_ulong

```synx
ulong? parse_ulong(&const str text)
```

Parses *text* as a decimal `ulong`. One leading `+` is allowed. All other
characters must be the digits `0` to `9`.

Returns `None` if *text* is empty, has no digits, contains another
character such as a space or a `-`, or is larger than
18446744073709551615. The range check is the same in debug and release
builds.

### parse_long

```synx
long? parse_long(&const str text)
```

Parses *text* as a decimal `long`. One leading `+` or `-` is allowed. All
other characters must be the digits `0` to `9`.

Returns `None` if *text* is empty, has no digits, contains another
character, or is outside the range -9223372036854775808 to
9223372036854775807. The range check is the same in debug and release
builds.

## Example

```synx
import std.text;

main() {
    var input = "name = Synx\r\nversion = 12\n\n";
    for (line in &const text.lines(input)) {
        match text.find(line, " = ") {
            None => {},
            Some(at) => {
                var key = text.slice(line, 0, at).unwrap();
                var value = text.slice(line, at + 3, line.len()).unwrap();
                match text.parse_long(value) {
                    Some(n) => print("{text.to_ascii_uppercase(key)} is the number {n}"),
                    None => print("{text.to_ascii_uppercase(key)} is the text {value}"),
                }
            }
        }
    }
    print(text.replace("a-b-c", "-", ", ").as_str());
}
```

Output:

```text
NAME is the text Synx
VERSION is the number 12
a, b, c
```
