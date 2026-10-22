# std.mem

```synx
import std.mem;
import std.mem.{drop, replace, swap};
```

This module moves values in and out of places without copying or dropping
them. The functions work on any type. They do not need `Copy`.

## Functions

### drop

```synx
void drop<T>(T value)
```

Takes ownership of *value* and drops it when the function returns. Use it to
run a destructor before the end of the scope. For a `Copy` type, the call
has no effect on the original value.

### replace

```synx
T replace<T>(&T place, T value)
```

Moves *value* into *place* and returns the previous value. The previous value
is not dropped. The caller owns it.

### swap

```synx
void swap<T>(&T left, &T right)
```

Exchanges the values at *left* and *right*. Neither value is copied or
dropped. To exchange two elements of one vector, use
[Vec.swap](prelude.md#vecswap).

## Example

```synx
import std.mem.{drop, replace, swap};

struct Ticket {
    int id;
}

impl Drop for Ticket {
    void drop(&self) { print("drop ticket {self.id}"); }
}

main() {
    var a = Ticket { id: 1 };
    var b = Ticket { id: 2 };
    swap(&a, &b);
    print("a = {a.id}, b = {b.id}");

    Ticket old = replace(&a, Ticket { id: 3 });
    print("old = {old.id}, a = {a.id}");

    drop(old);
    print("end of main");
}
```

The output is:

```text
a = 2, b = 1
old = 2, a = 3
drop ticket 2
end of main
drop ticket 1
drop ticket 3
```
