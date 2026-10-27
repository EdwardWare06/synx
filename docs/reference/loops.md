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
each iteration. The pattern must cover every possible element. Alternatives
can bind names under the [pattern rules](patterns.md). Parentheses are
required for `while` and `for`.

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

`for` also accepts types that implement `Iterator<T>`. The compiler calls
`T? next(&self)` until it returns `None`. The loop consumes an owned
iterator, or borrows a writable iterator supplied through `&iterator`.
A shared iterator reference cannot advance.

Iteration by value consumes a Vec and visits its elements in order. An early
exit drops unread elements. `&const vec` and `&vec` keep borrowed iteration.
A borrowed iterator item cannot remain in use when the loop advances again.
A break or return can preserve an item if its source and iterator loans
permit it. Borrowing from an iterator owned by the loop cannot escape that
owner's scope.
[std.iter](../library/iter.md) supplies adapters and borrowed slice iterators.
A while-condition temporary lives through that iteration's body and drops
before the next condition evaluation.
