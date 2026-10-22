# Patterns

Tests structure and binds matching values or references.

## Syntax

```text
_                           name
literal                     low..high              low..=high
Enum.Unit                   Enum.Tuple(pattern, pattern)
Type { field, other: pattern }
(pattern, pattern)           pattern | pattern
```

## Explanation

`_` matches without binding. A binding name matches and binds the input.
A resolved unit-variant name tests that variant instead. Literals test
equality; ranges test their bounds. Tuple and named-field patterns inspect
the corresponding parts. An alternative matches either of its patterns.

Bindings have arm or loop scope. In a by-value match, bindings move or copy
the selected part. In a reference match, bindings borrow that part.
Read-only access anywhere on the reference path gives a shared binding.

The current compiler rejects names bound within alternatives. Write
separate arms when alternatives need payload bindings. Guard expressions
use the bindings made by their own arm and must have type `bool`.

## Example

```synx
int group(int value) {
    return match value {
        1 | 2 => 1,
        3..=5 => 2,
        _ => 0,
    };
}

main() { print("{group(4)}"); }
```

Output: `2`.

## Notes

Local declarations use a binding name; this page does not introduce tuple
destructuring declarations. Match coverage has a current combination-check
limit. See [match](match.md).
