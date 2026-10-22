# 7. Ownership

In Java and Python, a variable holds a reference to an object, and many
variables can refer to the same object. A garbage collector frees the object
when no reference is left.

Synx has no garbage collector. Each value has exactly one owner, which is a
variable, a field, or an element of a container. When the owner goes out of
scope, the value is destroyed and its memory is freed. The compiler finds
the owner of each value when it compiles the program.

## Moves

Assignment of a `String` moves the value to the new variable. The old
variable can no longer be used:

```synx
main() {
    var playlist = String.from("Chokehold");
    var other = playlist;
    print(playlist);
}
```

```text
example.snx:4:11: error[SNX0501]: use of moved value `playlist`; it was moved earlier
4 |     print(playlist);
  |           ^^^^^^^^
1 error
```

After the move, `other` owns the text. If both variables could use the
string, both would free it at the end of `main`.

Passing a value to a function also moves it:

```synx
void consume(String s) {
    print(s);
}

main() {
    var s = String.from("a");
    consume(s);
    consume(s);
}
```

```text
example.snx:8:5: error[SNX0501]: use of moved value `s`; it was moved earlier
8 |     consume(s);
  |     ^^^^^^^^^^
1 error
```

A function can give ownership back by returning the value:

```synx
String shout(String text) {
    text.push_str("!");
    return text;
}

main() {
    var line = String.from("Lift");
    line = shout(line);
    print(line);
}
```

```text
Lift!
```

Moving a value in and out of every function is awkward. Usually a function
borrows the value instead. [Chapter 8](08-references.md) explains borrowing.

To make a second, independent string, build a new one from the first:

```synx
main() {
    var first = String.from("Granite");
    var copy = String.from(first);
    print(first);
    print(copy);
}
```

```text
Granite
Granite
```

## Copy types

Numbers, `bool`, `char`, and shared references are copied on assignment. The
old variable stays usable. A struct or enum is also copied when all of its
fields can be copied and it has no destructor:

```synx
struct Point {
    int x;
    int y;
}

main() {
    var a = Point { x: 1, y: 2 };
    var b = a;
    b.x = 10;
    print("{a.x} {b.x}");
}
```

```text
1 10
```

`b` is a separate copy, so the change to `b.x` does not change `a`. A struct
with a `String` field moves, because `String` cannot be copied.

## Destruction

At the end of a block, the values owned by the block are destroyed in
reverse order of declaration. A type can run code when it is destroyed. It
implements the `Drop` trait:

```synx
struct Plate {
    int pounds;
}

impl Drop for Plate {
    void drop(&self) {
        print("remove {self.pounds} lb plate");
    }
}

main() {
    var a = Plate { pounds: 45 };
    var b = Plate { pounds: 25 };
    {
        var c = Plate { pounds: 10 };
        print("inner block ends");
    }
    print("main ends");
}
```

```text
inner block ends
remove 10 lb plate
main ends
remove 25 lb plate
remove 45 lb plate
```

`String`, `Vec`, `Box`, files, and locks use `Drop` to free memory and close
handles. You do not call `drop` yourself. The compiler rejects a direct call.
To destroy a value early, pass it to `std.mem.drop`:

```synx
import std.mem;

struct Note {
    int id;
}

impl Drop for Note {
    void drop(&self) {
        print("drop {self.id}");
    }
}

main() {
    var a = Note { id: 1 };
    var b = Note { id: 2 };
    mem.drop(a);
    print("after drop");
    var moved = b;
    print("end");
}
```

```text
drop 1
after drop
end
drop 2
```

`b` moves to `moved`, so the value is destroyed once, at the end of `main`,
through its new owner.

## Panics and cleanup

A panic stops the program at once. Destructors do not run, and there is no
way to catch a panic. Use panics for bugs only. For failures that a program
can handle, use `Result`, which [chapter 9](09-errors.md) describes.

## Next

[Chapter 8](08-references.md) shows how to use a value without taking
ownership of it.
