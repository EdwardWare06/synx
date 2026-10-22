# if expression

Selects a block by a boolean condition.

## Syntax

```text
if (condition) { then_body }
if (condition) { then_body } else { else_body }
if (condition) { then_body } else if (other) { other_body }
```

## Explanation

The condition must have type `bool`. Parentheses around it are required.
The compiler does not convert integers or references to truth values.

The condition is evaluated once for that selection. If true, the then block
executes. Otherwise, the else branch executes when present. Unselected
branches have no runtime effects. Each block has its own scope.

When used as a value, the branches must produce compatible types. A branch
which returns or otherwise exits has no normal value to unify. A final
branch expression supplies its block's result; a trailing semicolon discards
that result.

## Example

```synx
int absolute(int value) {
    return if (value < 0) { -value } else { value };
}

main() {
    print("{absolute(-5)}");
}
```

Output: `5`.

## Notes

Negating the signed minimum follows the arithmetic-mode rules. The example
does not define a total absolute-value operation over every signed input.
Use [match](match.md) to inspect variants or bind payloads.
