# match expression

Selects an arm by comparing a value with patterns. The selected arm supplies
the result of the expression.

## Syntax

```text
match expression {
    pattern [if condition] => expression,
    pattern [if condition] => { statements }
}
```

Square brackets mark optional syntax. Separate expression arms with commas.
The comma after the final arm or a block arm is optional.

## Explanation

The operand is evaluated once. Arms are tested in source order. An arm is
selected when its pattern matches and its guard, if present, is true. A guard
must have type `bool`. Bindings from the pattern are available in the guard
and arm body.

The match must cover every possible input. Guarded arms do not count toward
coverage. For an integer operand, include a wildcard (`_`) or an unconditional
binding to cover the remaining values. For an enum, cover its variants or use
a wildcard.

Arms that produce values must have compatible result types. A `return`,
`break`, or `continue` arm can exit its enclosing function or loop.

Matching a reference binds references to its payloads. A shared operand gives
shared bindings; an exclusive operand gives exclusive bindings. Use `*` to
read a bound scalar.

## Example

```synx
enum Reading {
    Missing,
    Value(int),
}

int value_or_zero(&const Reading reading) {
    return match reading {
        Reading.Missing => 0,
        Reading.Value(value) => *value,
    };
}

main() {
    var reading = Reading.Value(7);
    print("{value_or_zero(reading)}");
}
```

Output:

```text
7
```

The call borrows `reading` through the shared parameter. In the second arm,
`value` has type `&const int`.

## Notes

Patterns include literals, bindings, `_`, enum variants, tuples, struct
fields, alternatives (`1 | 2`), and literal ranges (`3..=9`). Alternatives can
bind names when each alternative binds the same names and types. The first
matching alternative supplies the bindings. A false guard continues with the
next arm. Bindings are made before the guard. A false guard drops those
bindings. Later arms must follow the move rules for any fields already moved.

A by-value match follows the move and Copy rules. Use a reference operand
when the matched value must remain available.

Coverage preserves the combinations of fields in tuples, structs, and enum
variants, including nested alternatives. Covering each field separately is
insufficient. For example, `(true, false)` and `(false, true)` leave
`(true, true)` and `(false, false)` uncovered. Diagnostic `SNX0404` reports a
missing pattern. Literal and range patterns on scalar types other than
`bool` require a fallback arm.

Implementation: `selfhost/check.snx`, `selfhost/lower.snx`.
Regression cases: `tests/fixtures/patterns/`, `tests/patterns/coverage.py`,
and `tests/run/pattern_groups.snx`.
