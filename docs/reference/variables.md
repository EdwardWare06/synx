# Variable declaration

Declares a local binding with an explicit or inferred type.

## Syntax

```text
type name [= expression];
var name = expression;
const type name = expression;
const name = expression;
```

## Explanation

Typed and `var` bindings are writable. `const` prevents assignment to the
binding. Inference fixes a binding's type; subsequent assignments must have
that type. Numeric values require explicit conversions when their types differ.

A local must be initialized before it is read. The ownership checker follows
initialization through branches. An assignment can initialize an uninitialized
typed local. A by-value initialization moves its source unless the type is Copy.

A binding has block scope. Normal scope exit destroys its remaining owned
value. Shadowing introduces another binding; it does not assign to the old one.

## Example

```synx
main() {
    int count;
    count = 3;
    var step = 2;
    count += step;
    const limit = 5;
    print("{count == limit}");
}
```

Output: `true`.

## Notes

Binding mutability and reference access are separate properties. `&const T`
gives shared access to a value. `&T` gives exclusive access. See
[References](references.md) and [Constants](constants.md).
