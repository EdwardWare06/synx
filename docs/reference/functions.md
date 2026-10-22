# Function declaration

Defines a named callable with typed parameters.

## Syntax

```text
[pub] [return_type] name[<type_parameters>](parameters) { body }
```

Each ordinary parameter has the form `type name`. A function declaration has
no `fn` keyword. The `fn` keyword forms a function-value type.

## Explanation

A call evaluates its arguments and passes them by value, subject to Copy and
reference coercions. Owned arguments move into their parameters. A parameter
of type `&const T` permits an implicit shared borrow; `&T` requires an explicit
writable reference at an ordinary function call.

The written return type must match returned values. A function can return with
`return expression;` or use its final expression without a semicolon. The
semicolon after `return expression` is optional. A private
function can omit its return type when inference succeeds. Recursive functions,
public functions, and signatures without bodies require a written return type.

`void` is the type of no result. A function with no result expression can omit
its return type. A call cannot make a returned reference outlive its referent.

## Example

```synx
int sum(int left, int right) {
    return left + right;
}

twice(int value) { value * 2 }

main() {
    print("{twice(sum(2, 3))}");
}
```

Output: `10`.

## Notes

Use [generics](generics.md) for type parameters and trait bounds. Receiver
parameters belong to [methods](methods.md). Named functions can convert to
[function values](closures.md).
