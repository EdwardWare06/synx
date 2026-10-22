# 11. Traits and generics

A trait is a set of methods that a type can implement. It is like an
interface in Java. A generic function or struct has type parameters. A
bound on a type parameter names the traits that the type must implement.

## Declaring and implementing a trait

A trait declaration lists method signatures. An `impl Trait for Type` block
supplies the methods for one type:

```synx
trait Describe {
    String describe(&const self);
}

struct Lift {
    &const str name;
    int pounds;
}

impl Describe for Lift {
    String describe(&const self) {
        return "{self.name} at {self.pounds} lb";
    }
}

main() {
    var squat = Lift { name: "Squat", pounds: 315 };
    print(squat.describe());
}
```

```text
Squat at 315 lb
```

The methods in an `impl` block are separate from the methods in the struct
body. A type can implement many traits.

## Default methods

A trait method can have a body. An implementation can use that default or
replace it:

```synx
trait Track {
    &const str title(&const self);
    int seconds(&const self);
    String label(&const self) {
        return "{self.title()} ({self.seconds() / 60}:{self.seconds() % 60})";
    }
}

struct Song { &const str name; int length; }
struct Intro { int length; }

impl Track for Song {
    &const str title(&const self) { return self.name; }
    int seconds(&const self) { return self.length; }
}

impl Track for Intro {
    &const str title(&const self) { return "Intro"; }
    int seconds(&const self) { return self.length; }
    String label(&const self) { return String.from("Intro"); }
}

main() {
    var song = Song { name: "Somewhere I Belong", length: 213 };
    var intro = Intro { length: 20 };
    print(song.label());
    print(intro.label());
}
```

```text
Somewhere I Belong (3:33)
Intro
```

A trait can also be implemented for a built-in type such as `int`:

```synx
trait Describe {
    String describe(&const self);
}

impl Describe for int {
    String describe(&const self) {
        return "the number {*self}";
    }
}

main() {
    int reps = 8;
    print(reps.describe());
}
```

```text
the number 8
```

## Generic functions

A generic function lists its type parameters in `<...>` after its name. A
bound such as `T: Ord` says that `T` must implement `Ord`. The `+` sign
joins two bounds:

```synx
T larger<T: Ord + Copy>(T a, T b) {
    if (a < b) {
        return b;
    }
    return a;
}

main() {
    print("{larger(3, 9)}");
    print("{larger(2.5, 1.5)}");
    print("{larger<long>(7L, 4L)}");
}
```

```text
9
2.5
7
```

The compiler usually finds the type arguments from the arguments. When it
cannot, write them after the name, as in `larger<long>(...)`.

The compiler makes a separate copy of a generic function for each set of
type arguments.

A call with a type that does not meet a bound is an error:

```synx
struct Song { String title; }

T first<T: Copy>(&const Vec<T> items) {
    return items[0];
}

main() {
    var songs = Vec<Song>.new();
    songs.push(Song { title: String.from("Life Is Strange") });
    var song = first(&const songs);
}
```

```text
example.snx:10:16: error: the type 'Song' does not implement 'Copy', required for type argument 'T' of 'first'
10 |     var song = first(&const songs);
   |                ^^^^^
1 error
```

## Generic structs

A struct can have type parameters. A method in the struct body uses them
without a new declaration:

```synx
struct Stack<T> {
    Vec<T> items;

    Stack<T> new() {
        return Stack { items: Vec.new() };
    }

    void push(&self, T value) {
        self.items.push(value);
    }

    T? pop(&self) {
        return self.items.pop();
    }
}

main() {
    var plates = Stack<int>.new();
    plates.push(45);
    plates.push(25);
    print("{plates.pop() ?? 0}");
    print("{plates.pop() ?? 0}");
    print("{plates.pop() ?? 0}");
}
```

```text
25
45
0
```

`Stack<int>.new()` gives the type argument explicitly. A struct type
parameter can also have bounds:

```synx
struct Labeled<T: Display> {
    &const str label;
    T value;

    void show(&const self) {
        print("{self.label}: {self.value}");
    }
}

main() {
    var bench = Labeled { label: "bench", value: 225 };
    var bar = Labeled { label: "bar", value: String.from("standard") };
    bench.show();
    bar.show();
}
```

```text
bench: 225
bar: standard
```

## The core traits

The prelude declares these traits:

| Trait | Methods | Use |
|-------|---------|-----|
| `Eq` | `bool eq(&const self, &const Self other)` | `==` and `!=` |
| `Ord` | `bool lt(&const self, &const Self other)` | `<`, `>`, `<=`, `>=`, `sort` |
| `Display` | `String to_string(&const self)` | `{value}` in a string |
| `Add`, `Sub`, `Mul`, `Div`, `Rem` | `Self add(self, Self other)` and so on | `+ - * / %` |
| `Neg`, `Not` | `Self neg(self)`, `Self not(self)` | `-x`; `not` is called as a method |
| `Copy` | none | Copy instead of move |
| `Drop` | `void drop(&self)` | Code that runs when a value is destroyed |

`Self` is the type that implements the trait. The `Hash` trait for
`HashMap` keys is in [std.hash](../../library/hash.md).

## Comparison and Display

This program implements `Eq`, `Ord`, and `Display` for a struct. It then
uses the operators, string interpolation, and `sort`:

```synx
import std.collections.sort;

struct Weight {
    int pounds;
}

impl Eq for Weight {
    bool eq(&const self, &const Self other) {
        return self.pounds == other.pounds;
    }
}

impl Ord for Weight {
    bool lt(&const self, &const Self other) {
        return self.pounds < other.pounds;
    }
}

impl Display for Weight {
    String to_string(&const self) {
        return "{self.pounds} lb";
    }
}

main() {
    var a = Weight { pounds: 225 };
    var b = Weight { pounds: 185 };
    print("{a == b} {a != b} {a < b} {a > b} {a <= b} {a >= b}");

    Vec<Weight> sets = Vec.new();
    sets.push(a);
    sets.push(b);
    sets.push(Weight { pounds: 205 });
    sort(&sets);
    for (w in &const sets) {
        print("{w}");
    }
}
```

```text
false true false true false true
185 lb
205 lb
225 lb
```

On a user type, `a < b` calls `a.lt(b)` and `a > b` calls `b.lt(a)`. `<=`
and `>=` call `lt` and negate the result. The operands are evaluated from
left to right.

## Operator overloading

An implementation of `Add` enables `+`, and `Sub` enables `-`. The methods
take `self` by value:

```synx
struct Vec2 {
    int x;
    int y;
}

impl Add for Vec2 {
    Vec2 add(self, Vec2 other) {
        return Vec2 { x: self.x + other.x, y: self.y + other.y };
    }
}

impl Sub for Vec2 {
    Vec2 sub(self, Vec2 other) {
        return Vec2 { x: self.x - other.x, y: self.y - other.y };
    }
}

impl Display for Vec2 {
    String to_string(&const self) {
        return "({self.x}, {self.y})";
    }
}

main() {
    var waypoint = Vec2 { x: 10, y: 4 };
    var step = Vec2 { x: 2, y: -1 };
    var next = waypoint + step;
    var back = next - step - step;
    print("{next} {back}");
}
```

```text
(12, 3) (8, 5)
```

On a user type, `-x` calls `x.neg()`. The `!` operator accepts only `bool`.
To use `Not` on a user type, call `x.not()` as a method.

`Vec2` has only `int` fields and no `Drop`, so it is a Copy type. The
compiler makes a struct or enum Copy when every field is Copy and the type
has no `Drop`. You can write `impl Copy for Vec2 {}` as documentation.
[Chapter 7](07-ownership.md) explains Copy and `Drop`.

## Trait objects

A generic function has one copy for each type. A trait object selects the
method at run time. The type `&const dyn Trait` is a shared reference to a
value of any type that implements `Trait`. `&dyn Trait` is the writable
form.

```synx
trait Item {
    &const str name(&const self);
    int value(&const self);
}

struct Weapon { int damage; }
struct Quickhack { int ram; }

impl Item for Weapon {
    &const str name(&const self) { return "weapon"; }
    int value(&const self) { return self.damage * 10; }
}

impl Item for Quickhack {
    &const str name(&const self) { return "quickhack"; }
    int value(&const self) { return self.ram * 50; }
}

int total(&const Vec<&const dyn Item> items) {
    int sum = 0;
    for (item in items) {
        sum += item.value();
    }
    return sum;
}

main() {
    var rifle = Weapon { damage: 30 };
    var short_circuit = Quickhack { ram: 4 };
    Vec<&const dyn Item> loot = Vec.new();
    loot.push(&const rifle);
    loot.push(&const short_circuit);
    for (item in &const loot) {
        print("{item.name()}: {item.value()} eddies");
    }
    print("total: {total(&const loot)} eddies");
}
```

```text
weapon: 300 eddies
quickhack: 200 eddies
total: 500 eddies
```

A trait object borrows its value and cannot outlive it. The `Vec` holds
references, so `rifle` and `short_circuit` must live longer than `loot`.

A trait can be used with `dyn` only when each method has a `&const self` or
`&self` receiver, has no type parameters of its own, and uses `Self` only as
the receiver. `Eq`, `Ord`, and `Add` use `Self` in other places, so they
cannot be used with `dyn`.

## Next

[Chapter 12](12-closures.md) covers closures and function values.
