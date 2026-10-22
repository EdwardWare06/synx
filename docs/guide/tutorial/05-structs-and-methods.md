# 5. Structs and methods

## Declaring a struct

A struct groups named fields. Each field is written as a type and a name,
like a variable declaration:

```synx
struct Point {
    int x;
    int y;
}

main() {
    var p = Point { x: 3, y: 4 };
    p.x += 1;
    print("{p.x}, {p.y}");
}
```

```text
4, 4
```

A struct literal gives the type name and a value for each field, in braces.
There is no `new` keyword. Use `value.field` to read or change a field.

When a variable has the same name as a field, you can write the name alone:

```synx
struct Waypoint {
    &const str name;
    int x;
    int y;
}

main() {
    int x = 120;
    int y = -45;
    var w = Waypoint { name: "Afterlife", x, y };
    print("{w.name} at {w.x}, {w.y}");
}
```

```text
Afterlife at 120, -45
```

A struct literal must give every field. A missing field is an error:

```synx
struct Point {
    int x;
    int y;
}

main() {
    var p = Point { x: 1 };
}
```

```text
example.snx:7:13: error[SNX0406]: missing field 'y' in 'Point'
7 |     var p = Point { x: 1 };
  |             ^^^^^^^^^^^^^^
1 error
```

## Methods

Methods are declared inside the struct body, after the fields. The first
parameter is the receiver. It decides how the method gets the value:

```synx
struct Set {
    int reps;
    int pounds;

    int volume(&const self) {
        self.reps * self.pounds
    }

    void add_reps(&self, int extra) {
        self.reps += extra;
    }
}

main() {
    var bench = Set { reps: 5, pounds: 185 };
    print("{bench.volume()} lb");
    bench.add_reps(2);
    print("{bench.reps} reps, {bench.volume()} lb");
}
```

```text
925 lb
7 reps, 1295 lb
```

| Receiver | The method gets |
|----------|-----------------|
| `&const self` | A read-only reference to the value |
| `&self` | A writable reference to the value |
| `self` | The value itself |

Inside a method, `self.field` reads a field. The `self.` is required.

The call `bench.add_reps(2)` lends `bench` to the method. You do not write
`&` at the call. Chapters [7](07-ownership.md) and [8](08-references.md)
explain references and moves.

A `const` variable cannot call a method that takes `&self`:

```synx
struct Counter {
    int count;

    void increment(&self) {
        self.count += 1;
    }
}

main() {
    const c = Counter { count: 0 };
    c.increment();
}
```

```text
example.snx:11:7: error: method 'increment' changes the value, but the receiver is read only
11 |     c.increment();
   |       ^^^^^^^^^
1 error
```

## Methods that take self

A method with the receiver `self` takes the value. It often returns a new
value, so calls can follow each other:

```synx
struct Set {
    int reps;
    int pounds;

    Set heavier(self, int extra) {
        Set { reps: self.reps, pounds: self.pounds + extra }
    }
}

main() {
    var first = Set { reps: 5, pounds: 225 };
    var third = first.heavier(10).heavier(10);
    print("{first.pounds} then {third.pounds}");
}
```

```text
225 then 245
```

`first` is still usable after the call because `Set` is a Copy type. The
section on Copy below explains this.

## Associated functions

A function in the struct body without a receiver is an associated function.
Call it through the type name with a dot. Synx has no `::` operator:

```synx
struct Point {
    int x;
    int y;

    Point origin() {
        Point { x: 0, y: 0 }
    }

    Self at(int x, int y) {
        Self { x, y }
    }
}

main() {
    var a = Point.origin();
    var b = Point.at(2, 7);
    print("{a.x},{a.y} and {b.x},{b.y}");
}
```

```text
0,0 and 2,7
```

Inside the struct body, `Self` is another name for the struct type.
Associated functions take the place of constructors. A common name for one is
`new`.

All methods of a struct are declared in its body. Synx has no separate
`impl Point { ... }` block for them. The `impl` keyword is only for traits,
which [Chapter 11](11-traits-and-generics.md) covers.

## Copy structs

A struct is a Copy type when all of its fields are Copy types and it has no
`Drop` implementation. Numbers, `bool`, `char`, and `&const` references are
Copy types. Assignment of a Copy struct makes an independent copy:

```synx
struct Point {
    int x;
    int y;
}

main() {
    var a = Point { x: 1, y: 2 };
    var b = a;
    b.x = 10;
    print("a.x = {a.x}, b.x = {b.x}");
}
```

```text
a.x = 1, b.x = 10
```

A struct with a `String` field is not a Copy type. Assignment moves the
value, and the old variable can no longer be used:

```synx
struct Song {
    String title;
    int year;
}

main() {
    var first = Song { title: String.from("Somewhere I Belong"), year: 2003 };
    var second = first;
    var third = first;
}
```

```text
example.snx:9:17: error[SNX0501]: use of moved value `first`; it was moved earlier
9 |     var third = first;
  |                 ^^^^^
1 error
```

[Chapter 7](07-ownership.md) explains moves.

## Printing a struct

A struct cannot go into an interpolated string by itself:

```synx
struct Point {
    int x;
    int y;
}

main() {
    var p = Point { x: 1, y: 2 };
    print("{p}");
}
```

```text
example.snx:8:13: error: the type 'Point' does not implement 'Display', required for value in a string
8 |     print("{p}");
  |             ^
1 error
```

Print the fields, or implement the `Display` trait for the struct.
[Chapter 11](11-traits-and-generics.md) shows how to implement a trait.

## Public fields

Fields and methods are private to their module. Each file is a module. Code
in another file can use only the fields and methods marked `pub`:

```synx
// gear.snx
pub struct Plate {
    pub int pounds;
    int serial;

    pub Plate new(int pounds) {
        Plate { pounds, serial: 0 }
    }
}
```

A file that imports `Plate` can call `Plate.new(45)` and read `pounds`. A
use of `serial` in that file is an error with code SNX0304. Because `serial`
is private, the other file cannot write a `Plate { ... }` literal either. It
must call `Plate.new`. [Chapter 13](13-modules-and-files.md) covers modules.

## Next

[Chapter 6](06-enums-and-match.md) introduces enums and `match`.
