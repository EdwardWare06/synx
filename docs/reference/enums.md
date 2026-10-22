# Enum declaration

Defines a type with a fixed set of named variants.

## Syntax

```text
[pub] enum Name[<type_parameters>] {
    Unit,
    Tuple(type, type),
    Fields { type field; },
    methods
}
```

## Explanation

Each enum value has one active variant. A unit variant has no payload. A tuple
variant has positional payload fields. A field variant has named payload fields.
Use `Name.Variant` to select a variant and `match` to inspect its payload.

An enum can define methods. It is automatically Copy when every payload field
is Copy and it has no Drop implementation. A non-Copy payload follows ordinary
move and destruction rules. Only the active variant's payload is destroyed.

`Option<T>` and `Result<T, E>` are prelude enums. `T?` means `Option<T>`.
`Some`, `None`, `Ok`, and `Err` are available through the prelude.

## Example

```synx
enum Reply {
    Empty,
    Count(int),
}

main() {
    var reply = Reply.Count(8);
    match reply {
        Reply.Empty => print("empty"),
        Reply.Count(value) => print("{value}"),
    }
}
```

Output: `8`.

## Notes

Matching a reference binds references to payload fields. By-value matches can
move payloads. See [match](match.md) and [Patterns](patterns.md).
