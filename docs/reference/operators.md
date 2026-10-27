# Operators

Combines operands, selects storage, or changes a value.

## Syntax

The following groups run from lower to higher precedence. Operators on a
line have the same precedence.

```text
.. ..=
||
&&
== != < > <= >=
|
^
&
<< >>
+ -
* / %
as
unary: - ! ~ * & &const
postfix: call, index, ., ?., ?, ??, explicit type arguments
```

## Explanation

Use parentheses to make a required grouping explicit. `&&` and `||` use
boolean operands and short-circuit evaluation. Numeric arithmetic uses
matching operand types. No implicit widening occurs.

Integer add, subtract, multiply, negation, and signed minimum division by
minus one are checked by default. A failed check panics. `--release` wraps
the results to the type's width. Division or remainder by zero always
panics. Release masks shift counts; checked mode rejects counts outside
zero through width minus one. Left shifts discard high bits in both modes.

Assignment and compound assignment are statements ending in `;`. The
replacement is evaluated before the old owned destination is destroyed.
Member access uses `.`. Built-in indexing checks bounds in every arithmetic mode.
User indexing uses `Index<I, T>.index(&const self, I)` to borrow an element.
Assignment, compound assignment, writable borrows, and writable receiver calls
also require `IndexMut<I, T>.index_mut(&self, I)`. Index methods return
`&const T` and `&T`. The index expression is evaluated once. For assignment through a user index,
the replacement is evaluated before the index receiver is borrowed. This also
applies to the right operand of compound assignment.
A non-Copy element cannot be moved through the returned reference.

Slices use `&const values[start..end]` or `&values[start..end]`.
The result is a shared or writable `T[]` view. The start defaults to zero
and an omitted exclusive end defaults to the collection length. Inclusive
ends use `..=` and must be present. Invalid or reversed bounds panic in
all arithmetic modes. Empty slices at the end of a collection are valid.
A slice keeps its source borrowed while the slice is in use. Arrays,
slices, and Vec support slicing. Strings use the UTF-8 methods in `std.text`.
Use `slice.len()` for the element count.

Optional access `value?.field` returns `Option<FieldType>`.
`value?.method(args)` calls the method only for `Some`. The base is
evaluated once. `None` skips the arguments. A field that contains an
Option produces a nested Option. Copy fields leave the owner usable;
moving a non-Copy field or calling a consuming method moves the owner.
References retain the normal access and lifetime checks.
An optional method borrows or moves its receiver before evaluating its
arguments. An argument cannot invalidate the tested payload.

## Example

```synx
main() {
    int value = (2 + 3) * 4;
    bool valid = value == 20 && value > 0;
    print("{valid}");
}
```

Output: `true`.

## Notes

Core traits include Add, Sub, Mul, Div, Rem, Eq, Ord, Neg, and Not.
Implementations enable their supported operator forms. The grammar assigns
`??` postfix parsing; use parentheses around compound fallback expressions.
See [Errors](errors.md) and [Conversions](conversions.md).
