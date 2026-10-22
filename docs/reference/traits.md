# Trait declaration and implementation

Defines a set of methods which a type can implement.

## Syntax

```text
[pub] trait Name[<type_parameters>] {
    return_type method(parameters);
    return_type default_method(parameters) { body }
}
impl[<type_parameters>] Trait[<arguments>] for Type { methods }
&const dyn Trait
&dyn Trait
```

## Explanation

A trait method can have a default body. An implementation supplies the
required methods for a concrete type or generic family. A generic bound
`T: Trait` requires that implementation at instantiation.

A borrowed trait object selects methods at runtime. Its current representation
contains a data pointer and a vtable pointer. A shared trait-object reference
permits shared methods. An exclusive reference permits writable methods too.
The object borrows its data and cannot outlive it.

Every method of a trait used with `dyn` must have a reference receiver, have
no method type parameters, and use `Self` only as the receiver. Associated
functions and by-value receivers make the trait ineligible for `dyn`.

## Example

```synx
trait Read {
    int read(&const self);
}
struct Number { int value; }
impl Read for Number {
    int read(&const self) { return self.value; }
}
int read_value(&const dyn Read source) { return source.read(); }

main() {
    var number = Number { value: 9 };
    print("{read_value(number)}");
}
```

Output: `9`.

## Notes

`Send`, `Sync`, and `Owned` are compiler-checked marker traits. An explicit
implementation of one requires `unsafe impl` and a contract the author must
uphold. Owned trait objects and Send-qualified trait objects are outside the
current interface. See [Generics](generics.md) and [unsafe](unsafe.md).
