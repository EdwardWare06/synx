# Type parameters

Defines functions and nominal types over concrete type arguments.

## Syntax

```text
return_type name<T, U: Trait + OtherTrait>(parameters) { body }
struct Name<T> { members }
enum Name<T> { members }
trait Name<T> { members }
name<Type>(arguments)
```

## Explanation

A type parameter denotes a type. Bounds require trait implementations for
each substituted argument. The compiler checks generic bodies and checks
instantiations against their bounds. It lowers concrete function instances
as they are needed, a process called monomorphization.

Type arguments can be inferred from arguments and expected types. Use an
explicit argument list when inference has insufficient information. At a
call, the parser recognizes `<...>` as type arguments when the closing `>`
is followed by `(` or `.`. Otherwise, `<` remains a comparison operator.

All type arguments must be sized. Bare `str`, slices, and `dyn Trait` cannot
be generic arguments. Their reference forms are sized and can be arguments.

## Example

```synx
T larger<T: Ord>(T left, T right) {
    if (left > right) { return left; }
    return right;
}

main() {
    print("{larger<int>(3, 7)}");
}
```

Output: `7`.

## Notes

Monomorphization can increase object size. It removes the need for a vtable
at a generic call, while a `dyn` reference selects an implementation at
runtime. There is no general optimization guarantee attached to either form.
Recursive type expansion is bounded and can produce a compiler diagnostic.
