# std.iter

```synx
import std.iter;
```

Iterator constructors and adapters. `Iterator<T>` is also in the prelude.
Each iterator owns its state. `next(&self)` returns `Some(item)` or `None`.
Integer `Range` and `RangeInclusive` implement Iterator for byte, short, int,
long, uint, ulong, and usize. An exhausted integer range stays exhausted.

## Borrowed iteration

```synx
SliceIter<T> iter<T>(&const T[] values)
SliceIterMut<T> iter_mut<T>(&T[] values)
```

`SliceIter<T>` yields `&const T`. `SliceIterMut<T>` yields `&T`.
Both keep the source borrowed. An item retains its iterator loan.
Do not call `next` again while a previous borrowed item is still in use.
A `for` loop checks that no borrowed item remains in use when it advances.
An item can leave through break or return if the normal lifetime rules allow it.

Use `&const values[..]` or `&values[..]` to pass a whole collection as a
slice. Shared array and Vec arguments can also coerce to shared slices.

## map

```synx
Map<I, T, U> map<T, U, I: Iterator<T>>(I source, fn(T) -> U transform)
```

Consumes source and transform. Each next call applies transform to one item.
The adapter implements `Iterator<U>`. It preserves the source order.

## filter

```synx
Filter<I, T> filter<T, I: Iterator<T>>(I source, fn const(&const T) -> bool predicate)
```

Consumes source and predicate. Next discards items until predicate returns
true or the source ends. Rejected owned items are dropped. Predicate is a
shared function value, so it cannot retain the temporary borrow of an item
through writable captures.

## enumerate

```synx
Enumerate<I, T> enumerate<T, I: Iterator<T>>(I source)
```

Yields `(usize, T)` pairs with indices starting at zero. Index overflow
panics in every arithmetic mode.

## collect

```synx
Vec<T> collect<T, I: Iterator<T>>(I source)
```

Consumes the iterator into a Vec in source order. Items must be valid after
the iterator advances. Borrowed items tied to the current iterator call
cannot be collected.

## Example

```synx
import std.iter;
main() {
    var doubled = iter.map(1..4, |int n| { return n * 2; });
    for ((index, value) in iter.enumerate(doubled)) {
        print("{index}: {value}");
    }
}
```

Output:

```text
0: 2
1: 4
2: 6
```
