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

Every alternative must bind the same names with the same types and reference
access. For example, `Choice.A(value) | Choice.B(value)` binds one `value`.
Names can occur at different field positions. The first matching alternative
supplies the bindings. All tests on the arm pattern finish before bindings
move or borrow fields. Each name can occur once on a path through the pattern.
A named field can occur once in a struct or variant pattern.

Guard expressions use the bindings made by their own arm and must have type
`bool`. If a guard is false, testing continues with the next arm. By-value
move bindings consume their fields before the guard. Later arms cannot read
those fields. Match a reference when a later arm needs the payload.

## Example

```synx
int group(int value) {
    return match value {
        1 | 2 => 1,
        3..=5 => 2,
        _ => 0,
    };
}

enum Input { Local(int), Remote(int) }

int input_value(Input input) {
    return match input { Input.Local(value) | Input.Remote(value) => value };
}

main() {
    print("{group(4)}");
    print("{input_value(Input.Remote(7))}");
}
```

Output:

```text
2
7
```

## Notes

Local declarations use a binding name; this page does not introduce tuple
destructuring declarations. A `for` pattern must match every possible element.
See [match](match.md) for coverage rules.
