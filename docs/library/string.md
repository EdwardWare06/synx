# std.string

```synx
import std.string.String;
```

This module re-exports the type `String` from the prelude. It defines no
other items. `String` is in the prelude, so a program can use it with no
import. For the rules of `String` and `str`, see
[String expressions](../reference/strings.md). For text operations, see
[std.text](text.md).

## Example

```synx
import std.string.String;

main() {
    var name = String.from("Synx");
    var greeting = String.new();
    greeting.push_str("hello, ");
    greeting.push_str(name.as_str());
    greeting.push('!');
    print(greeting);
    print("{greeting.len()} bytes");
}
```

Output:

```text
hello, Synx!
12 bytes
```
