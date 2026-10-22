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
postfix: call, index, ., ?, ??, explicit type arguments
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
Member access uses `.`. Indexing checks bounds in every arithmetic mode.

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
Implementations enable their supported operator forms. Indexing works on
arrays, slices, and Vec only; user types cannot overload it. The grammar assigns
`??` postfix parsing; use parentheses around compound fallback expressions.
See [Errors](errors.md) and [Conversions](conversions.md).
