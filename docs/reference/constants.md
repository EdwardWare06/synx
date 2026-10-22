# Constant declaration

Declares an immutable local or a named module-level initializer.

## Syntax

```text
const type name = expression;
const name = expression;
pub const type name = expression;
```

## Explanation

A local constant is an immutable binding. A module-level constant provides a
named initializer which the current compiler evaluates at each use. An owned
initializer creates a fresh value each time. A closure initializer creates a
fresh environment each time.

The initializer can contain a block and local declarations. It cannot return
from its caller, use `?` to propagate a failure, or use `await`.

`pub` exports a module item. An unqualified use resolves within the current
scope, including its imports. A local declaration can shadow a prelude name.

## Example

```synx
const int STEP = 4;

main() {
    const start = 6;
    print("{start + STEP}");
}
```

Output: `10`.

## Notes

Module constants currently have no general compile-time evaluation guarantee.
Do not assume that an allocating initializer refers to one shared allocation.
`const` does not add `Copy` to a type.

See [Variables](variables.md) and [Ownership](ownership.md).
