# Types

Determines a value's operations, storage, and ownership behavior.

## Syntax

```text
bool byte short int long uint ulong usize float double char void
Name<arguments>
T[N]       T[]       (T, U)       (T,)
&T         &const T  *T           *const T
T?         fn(T) -> U            &const dyn Trait
Box<dyn Trait>
```

## Explanation

`byte` is unsigned 8-bit; `short`, `int`, and `long` are signed 16-, 32-,
and 64-bit integers. `uint` and `ulong` are unsigned 32- and 64-bit integers.
`usize` has pointer width, currently 64 bits. `float` and `double` are IEEE
754 binary32 and binary64. `bool` uses one byte. `char` uses four bytes and
represents a Unicode scalar value. `void`, also written `()`, has no payload.

Arrays have a fixed element count. Slices are unsized views used through a
reference. Tuples store fields selected as `.0`, `.1`, and so on. `(T,)` is
a one-field tuple; `(T)` groups a type.

`T?` is `Option<T>` and applies to the whole preceding type. Thus
`&const str?` is `Option<&const str>`. Use `&const Option<T>` to borrow
an Option itself. Reference and pointer access qualifiers belong to the
referenced type. Bare `str`, `T[]`, and `dyn Trait` are unsized.
`Box<dyn Trait>` is a sized owner containing a data pointer and a vtable
pointer. See [Traits](traits.md).

## Example

```synx
main() {
    int[3] values = [2, 4, 6];
    (int, bool) pair = (values[1], true);
    int? optional = Some(pair.0);
    print("{optional.unwrap()}");
}
```

Output: `4`.

## Notes

Numeric types do not implicitly widen. See [Conversions](conversions.md).
Storage location follows use and ownership; an array can be a local, field,
or heap payload. Its type alone does not require stack allocation.
