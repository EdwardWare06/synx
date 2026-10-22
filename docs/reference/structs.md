# Struct declaration

Defines a named type with fields and optional methods.

## Syntax

```text
[pub] struct Name[<type_parameters>] {
    [pub] type field;
    methods
}
Name { field: expression, other_field }
```

The second form constructs a value. A field name without `: expression` uses
the value of the binding with that name.

## Explanation

Fields have declared types and declaration order. Fields and inherent methods
are private unless marked `pub`. Construction and member access must respect
module visibility. Use `value.field` for field access.

A struct is Copy when all fields are Copy and it has no Drop implementation.
Otherwise, passing or assigning it by value transfers ownership. A struct can
store references; its owner then carries the loans of the referenced values.

An inline recursive layout is invalid. Put an owning recursive edge behind a
sized allocation, such as `Box<Node>` or `Vec<Node>`. References and raw pointers
also break a layout cycle, with their own lifetime requirements.

## Example

```synx
struct Point {
    int x;
    int y;
}

main() {
    int x = 2;
    var point = Point { x, y: 5 };
    point.x += 1;
    print("{point.x}, {point.y}");
}
```

Output: `3, 5`.

## Notes

The language has no class inheritance. Use fields for composition and
[traits](traits.md) for shared interfaces. Current x86-64 field layout uses
declaration order, natural alignment, and tail padding. The native ABI remains
an implementation detail; use the supported C boundary for external calls.
