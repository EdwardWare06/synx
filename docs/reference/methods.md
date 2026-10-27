# Method declaration

Defines an operation within a struct, enum, or trait implementation.

## Syntax

```text
return_type name(&const self, parameters) { body }
return_type name(&self, parameters) { body }
return_type name(self, parameters) { body }
return_type name(parameters) { body }
```

## Explanation

`&const self` borrows the receiver for shared access. `&self` borrows it for
exclusive access. `self` takes the receiver by value and therefore moves it
unless its type is Copy. A member function with no receiver is an associated
function, called through its type.

Receiver borrowing is automatic. A call to `value.update()` can borrow
`value` exclusively even though no `&` appears at the call. This rule is
specific to the receiver; writable ordinary arguments still require `&`.

Call arguments are evaluated before the receiver is borrowed. A reference
returned from a method can retain a receiver loan after the call ends.
An exclusive receiver cannot be used through another path while that loan
conflicts with the use.

## Example

```synx
struct Counter {
    int value;
    Counter new() { return Counter { value: 0 }; }
    void add(&self, int amount) { self.value += amount; }
    int read(&const self) { return self.value; }
}

main() {
    var counter = Counter.new();
    counter.add(3);
    print("{counter.read()}");
}
```

Output: `3`.

## Notes

Use `.` for both associated functions and receiver calls. There is no `::`
member operator. Trait implementations use the same receiver forms. Borrowed
trait objects call reference receivers; `Box<dyn Trait>` also calls consuming
receivers. See [Traits](traits.md).
