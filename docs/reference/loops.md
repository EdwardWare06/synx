# Loop statements

Repeats a block unconditionally, by condition, or over a supported sequence.

## Syntax

```text
while (condition) { body }
loop { body }
for (pattern in expression) { body }
break;
continue;
```

## Explanation

`while` tests a `bool` condition before each iteration. `loop` repeats until
control leaves it. `for` evaluates its sequence and binds its pattern on
each iteration. Parentheses are required for `while` and `for`.

Integer `start..end` ranges exclude the end. `start..=end` includes it.
An inclusive range stops before incrementing past its final value, including
the largest representable integer. `continue` on that final iteration also
stops the range.

`break` leaves the innermost loop. `continue` advances to its next iteration.
Both perform the cleanup for scopes they leave. Borrow a collection for
iteration when its elements must remain owned by the collection. Shared
iteration binds shared element references.

## Example

```synx
main() {
    int total = 0;
    for (value in 1..=4) {
        total += value;
    }
    print("{total}");
}
```

Output: `10`.

## Notes

The current compiler lowers supported range and collection loops directly.
Do not infer an arbitrary user-defined iterator protocol from the `for`
syntax. A while-condition temporary lives through that iteration's body and
drops before the next condition evaluation.
