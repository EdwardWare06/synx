# Closures and function values

Stores callable code with an owned capture environment.

## Syntax

```text
fn(A) -> R                 |A value| expression
fn const(A) -> R           const |A value| expression
fn once(A) -> R            once |A value| expression
[&const name, &other, owned] || { body }
fn once send() -> R        send once || expression
```

Capture-list brackets are literal syntax. `||` denotes no parameters.

## Explanation

A plain closure captures Copy values by copy and other values by move.
An explicit list names every outer local used by the body. `&const name`
borrows its storage for shared access; `&name` borrows exclusively; a plain
name copies or moves. Captured references keep the referents borrowed.

`fn` calls borrow the function value exclusively and can change captures.
`fn const` calls borrow it for shared access and cannot change or move
captures. `fn once` calls consume it and can move captured owners out.
All function values move, including values made from named functions.

A shared function value can become writable. Named functions can adapt to
all modes. Other mode conversions are rejected. A send function type
retains a checked Send and Owned guarantee for its hidden captures. A plain
function type cannot recover that guarantee after it has been erased.

## Example

```synx
main() {
    int count = 0;
    {
        var next = [&count] || { count += 1; count };
        print("{next()}");
    }
    print("{count}");
}
```

Output:

```text
1
1
```

## Notes

Arguments are evaluated before the call borrows or consumes the callee.
Implicit captures use first-use order; explicit captures use list order.
Cleanup reverses capture order. Loans follow nested and stored closures.
Builtin and extern functions cannot currently be stored as function values.
