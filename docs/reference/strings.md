# String expressions

Represents UTF-8 text as borrowed or owned storage.

## Syntax

```text
"literal text"
"text {expression} text"
String.from(borrowed_text)
```

## Explanation

A plain literal is `&const str` and has program lifetime. `str` is unsized;
its reference holds an address and byte length. `String` owns a growable
allocation. `String.from` copies borrowed text into owned storage. An
interpolated literal produces an owned String and formats its expressions.

Lengths and text offsets are measured in UTF-8 bytes. A `char` denotes one
Unicode scalar value. Byte length, scalar count, and displayed character
count can differ. Use `std.text` and `std.utf8` for text operations and
validated boundaries. Arbitrary byte offsets can split a multi-byte scalar.

`String` moves when passed by value. A shared string parameter borrows it
implicitly. A borrowed view must not outlive its String or overlap a
conflicting mutation. Use `as_str()` for an explicit shared view.

## Example

```synx
main() {
    var name = String.from("Ada");
    var message = "Hello, {name}";
    print(message);
}
```

Output: `Hello, Ada`.

## Notes

`print`, `format`, and `panic` are available from the prelude. Formatting
uses Display support for the formatted type. See the standard-library
reference for the full String and text APIs. Raw text construction belongs
to the unsafe interface and requires valid storage and UTF-8.
