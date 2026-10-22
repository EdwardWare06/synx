# Error values and panics

Represents expected failure with a value and stops execution on a panic.

## Syntax

```text
Result<T, E>      Ok(value)      Err(error)
Option<T>        Some(value)    None
expression?
expression ?? fallback
panic(message)
```

## Explanation

`Result<T, E>` holds success or error. `Option<T>` holds a value or absence.
Match these enums to select behavior explicitly. Postfix `?` unwraps
success or presence; failure returns from the enclosing function. A Result
propagation requires a compatible Result return and error type; an Option
propagation requires an Option return.

`??` selects a present payload or its fallback. These forms follow normal
ownership and reference rules.

`panic` prints `Synx panic: ` and a message, then ends the process with
status 101. There is no catch, unwinding, or promised stack trace. Bounds
failures and zero division panic in every arithmetic mode.

## Example

```synx
enum Error { Negative }

Result<int, Error> positive(int value) {
    if (value < 0) { return Err(Error.Negative); }
    return Ok(value);
}
Result<int, Error> calculate() {
    int value = positive(4)?;
    return Ok(value * 2);
}
main() {
    match calculate() {
        Ok(value) => print("{value}"),
        Err(_) => print("error"),
    }
}
```

Output: `8`.

## Notes

Optional member access `expression?.field` is reserved. The parser and
type checker accept it, but code generation rejects it with SNX0601. Use
`match` or `??` instead.

Panics do not run destructors. A worker-thread panic stops the process.
The compiler reports diagnostics separately with SNX-prefixed codes.
Use Result for failures the caller is expected to handle.
