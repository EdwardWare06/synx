# Casts and coercions

Converts a value explicitly or adapts it at a typed use.

## Syntax

```text
expression as Type
```

## Explanation

Numeric casts are explicit. Integer casts retain the destination width,
truncating when it is narrower. The arithmetic mode does not turn an
explicit truncating integer cast into an overflow check. Literal casts
can explicitly request truncation too.

Reference coercions preserve valid access and lifetimes. A shared parameter
can borrow a value, adapt String to shared str, adapt an array or Vec to
a shared slice, or adapt a concrete implementing value to a compatible
borrowed trait object. Writable parameters require explicit references
at ordinary calls.

`Box<T>` coerces to `Box<dyn Trait>` when `T` implements `Trait`. The conversion
moves the box and preserves its allocation. It applies at typed assignments,
arguments, returns, and container insertions. Build the concrete value with
`Box.new(value)` first; `Box<dyn Trait>.new(value)` cannot construct an unsized
payload. An owned trait object retains any loans carried by the concrete
value. See [Traits](traits.md).

A sized `&T` can cast to `*T` or `*const T`. A sized `&const T` can cast
to `*const T`. A shared reference cannot cast directly to a writable raw
pointer. Casting an exclusive reference transfers that reference's value;
create a fresh borrow when the original reference is still needed.

## Example

```synx
main() {
    int value = 300;
    byte low = value as byte;
    long wide = low as long;
    print("{wide}");
}
```

Output: `44`.

## Notes

Raw pointer lifetimes are the unsafe caller's responsibility. Unsized
reference-to-pointer casts are currently unsupported. A cast does not
validate UTF-8, repair an invalid pointer, or authorize an aliasing conflict.
Use library parsing functions for checked conversion from text.
