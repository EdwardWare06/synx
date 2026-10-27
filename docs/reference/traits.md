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
Box<dyn Trait>
```

## Explanation

A trait method can have a default body. An implementation supplies the
required methods for a concrete type or generic family. A generic bound
`T: Trait` requires that implementation at instantiation.

A borrowed trait object selects methods at runtime. Its current representation
contains a data pointer and a vtable pointer. A shared trait-object reference
permits shared methods. An exclusive reference permits writable methods too.
The object borrows its data and cannot outlive it.

Every method of a trait used with `dyn` must have a receiver, have no method
type parameters, and use `Self` only as the receiver. Associated functions
make the trait ineligible for `dyn` (`SNX0410`).

`Box<dyn Trait>` owns its concrete value and allocation. A `Box<T>` coerces
to `Box<dyn Trait>` when `T` implements the trait. This conversion moves the
box and preserves the allocation. The erased value can contain borrowed
references; their lifetimes remain checked through moves and containers.

Call trait methods directly on the owned box. Shared and writable methods
borrow the payload. A method with `self` consumes the box, moves its concrete
payload into the method, and frees the allocation. A borrowed box or borrowed
trait object cannot call a consuming method. The box cannot be used after a
consuming call. Ordinary method arguments are evaluated before the receiver
is borrowed or consumed.

`as_ref()` and `as_mut()` return borrowed trait objects. Their loans keep the
owner alive. Dropping an owned trait object runs the concrete value's full
cleanup, then frees its allocation. Methods and explicit trait implementations
on `Box<dyn Trait>` take priority over payload methods of the same name.

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

## Owned example

```synx
trait Finish { String finish(self); }
struct Message { String text; }
impl Finish for Message {
    String finish(self) { return self.text; }
}

main() {
    Box<dyn Finish> message = Box.new(Message { text: String.from("ready") });
    print(message.finish());
}
```

Output:

```text
ready
```

## Notes

`Send`, `Sync`, and `Owned` are compiler-checked marker traits. An explicit
implementation of one requires `unsafe impl` and a contract the author must
uphold. Erasure does not preserve `Send`, `Sync`, or `Owned`: an ordinary
`Box<dyn Trait>` does not satisfy these marker bounds. Qualified trait objects
such as `dyn Trait + Send` are not supported.

An owned trait object does not automatically implement its payload trait for
generic bounds. Use its borrowed accessors for parameters such as
`&const dyn Trait`. `Box<str>` and `Box<T[]>` are not supported. `Box.new` and
`into_inner` require a sized payload. See [Generics](generics.md),
[Box](../library/prelude.md#box), and [unsafe](unsafe.md).
