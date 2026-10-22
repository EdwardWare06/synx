# Ownership and destruction

Controls transfer and cleanup of values.

## Syntax

```text
destination = source;
impl Copy for Type { }
impl Drop for Type { void drop(&self) { body } }
```

## Explanation

A non-Copy value has one owner. Assignment, by-value arguments, returns, and
captures can transfer ownership. A moved source cannot be read until it is
initialized again. Primitive values, shared references, and aggregates of
Copy fields copy. A struct or enum with a Drop implementation is move-only.
Function values and owning library containers move.

Normal scope exit drops live owned locals in reverse declaration order.
Cleanup calls the user Drop method, then destroys fields in declaration
order. Array and Vec elements drop in index order. Closures drop captures
in reverse capture order. Conditional initialization uses drop flags.

Assignment evaluates its replacement first, then drops the old destination,
then stores the replacement. An old value moved during evaluation is not
dropped again. `std.mem.replace`, `swap`, and `drop` provide explicit transfer
and cleanup operations. Calling the Drop trait method directly is rejected.

## Example

```synx
import std.mem;

main() {
    var text = String.from("first");
    var old = mem.replace(&text, String.from("second"));
    print(old);
    print(text);
}
```

Output:

```text
first
second
```

## Notes

Moving a field currently makes its whole local unavailable. Remaining
unmoved fields are cleaned up; the partially moved owner's Drop method
does not run. Deep recursive destruction uses the native stack. `Box<T>`
supports sized recursive ownership and destroys its payload before freeing
storage. A panic aborts without unwinding or cleanup.
