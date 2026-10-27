# 6. Enums and match

## Enums

An enum is a type with a fixed set of variants. Write a variant as
`Type.Variant`. `match` selects code by variant:

```synx
enum Lift {
    Squat,
    Bench,
    Deadlift,
}

main() {
    var lift = Lift.Bench;
    match lift {
        Lift.Squat => print("legs"),
        Lift.Bench => print("chest"),
        Lift.Deadlift => print("back"),
    }
}
```

```text
chest
```

Each arm has a pattern, `=>`, and an expression. Commas separate the arms.

Enums do not have `==` by default. `lift == Lift.Bench` is an error unless
the enum implements the `Eq` trait. Use `match` to test a variant.

## Variants with data

A variant can carry values. A tuple variant has values by position. A
struct variant has named fields:

```synx
enum Shape {
    Dot,
    Circle(double),
    Rect { double w; double h; },
}

main() {
    Shape[3] shapes = [Shape.Dot, Shape.Circle(2.0), Shape.Rect { w: 3.0, h: 4.0 }];
    for (s in shapes) {
        match s {
            Shape.Dot => print("a dot"),
            Shape.Circle(r) => print("a circle of radius {r}"),
            Shape.Rect { w, h } => print("a {w} by {h} rectangle"),
        }
    }
}
```

```text
a dot
a circle of radius 2.0
a 3.0 by 4.0 rectangle
```

A pattern such as `Shape.Circle(r)` binds the value inside the variant to
the name `r`. That name is available in the arm.

An enum is a Copy type when all of its values are Copy types and it has no
`Drop` implementation. `Shape` is a Copy type, so the loop can copy each
element out of the array.

## Methods on enums

An enum can have methods and associated functions, like a struct. Declare
them after the variants:

```synx
enum Shape {
    Circle(double),
    Rect { double w; double h; },

    double area(&const self) {
        match self {
            Shape.Circle(r) => 3.14159 * *r * *r,
            Shape.Rect { w, h } => *w * *h,
        }
    }

    Shape square(double side) {
        Shape.Rect { w: side, h: side }
    }
}

main() {
    print("{Shape.square(3.0).area()}");
    print("{Shape.Circle(1.0).area()}");
}
```

```text
9.0
3.14159
```

`self` is a reference here, so `r`, `w`, and `h` are references too. The `*`
reads the value that a reference points to. The section on references at
the end of this chapter explains this rule.

## match as an expression

`match` gives the value of the arm that runs:

```synx
enum Rarity {
    Common,
    Rare,
    Legendary,
}

int sell_price(Rarity rarity) {
    match rarity {
        Rarity.Common => 10,
        Rarity.Rare => 150,
        Rarity.Legendary => 2000,
    }
}

main() {
    print("{sell_price(Rarity.Rare)} eddies");
}
```

```text
150 eddies
```

All arms must give the same type. An arm can be a block in braces. The last
expression of the block is the value of the arm.

## Patterns

A pattern can be a literal, a range of literals, several patterns joined by
`|`, a name, or `_`:

```synx
&const str describe(int reps) {
    match reps {
        0 => "no reps",
        1 => "a single",
        2 | 3 => "low reps",
        4..=12 => "a normal set",
        n => if (n > 30) { "too many" } else { "high reps" },
    }
}

main() {
    for (reps in [0, 1, 3, 8, 20, 50]) {
        print("{reps}: {describe(reps)}");
    }
}
```

```text
0: no reps
1: a single
3: low reps
8: a normal set
20: high reps
50: too many
```

Arms are tested from top to bottom. The first arm that matches runs.

- A literal matches an equal value. Numbers, characters, `bool`, and string
  literals are permitted.
- `a..b` and `a..=b` match a range, with the same ends as `for` ranges.
- `p | q` matches when either pattern matches.
- A name matches any value and binds it to that name.
- `_` matches any value and binds nothing.

A guard is an `if` condition after the pattern. The arm runs only when the
pattern matches and the guard is true:

```synx
main() {
    int[4] temps = [-5, 0, 18, 31];
    for (t in temps) {
        var feel = match t {
            n if n < 0 => "freezing",
            0 => "zero",
            n if n > 30 => "hot",
            _ => "mild",
        };
        print("{t}: {feel}");
    }
}
```

```text
-5: freezing
0: zero
18: mild
31: hot
```

Patterns can be nested. A tuple pattern matches each field of a tuple:

```synx
main() {
    (int, int)[4] points = [(0, 0), (0, 5), (3, 0), (2, 2)];
    for (p in points) {
        match p {
            (0, 0) => print("origin"),
            (0, y) => print("on the y axis at {y}"),
            (x, 0) => print("on the x axis at {x}"),
            (x, y) => print("at {x}, {y}"),
        }
    }
}
```

```text
origin
on the y axis at 5
on the x axis at 3
at 2, 2
```

A struct pattern names the fields that it tests. It can omit other fields:

```synx
struct Track {
    &const str title;
    int year;
}

main() {
    var t = Track { title: "Take Me Back to Eden", year: 2023 };
    match t {
        Track { year: 2020..=2029, title } => print("{title} is from the 2020s"),
        Track { title } => print("{title}"),
    }
}
```

```text
Take Me Back to Eden is from the 2020s
```

The alternatives of a `|` pattern can bind names. Each alternative must bind
the same names with the same types. For example, `Choice.Left(n) |
Choice.Right(n)` binds one `n` when both variants carry the same type.
`Some(n) | None` is an error because `None` does not bind `n`. The first
matching alternative supplies the bindings.

## Exhaustive matches

A `match` must cover every possible value. A missing variant is an error:

```synx
enum Shape {
    Dot,
    Circle(double),
    Rect { double w; double h; },
}

main() {
    var s = Shape.Circle(2.0);
    var size = match s {
        Shape.Dot => 0.0,
        Shape.Circle(r) => r,
    };
}
```

```text
example.snx:9:16: error[SNX0404]: match is not exhaustive; missing: Rect { w: _, h: _ }
9 |     var size = match s {
  |                ^^^^^^^^^
1 error
```

The compiler cannot list the missing values of an integer. A match on an
integer needs a `_` arm or a name arm at the end:

```synx
main() {
    int reps = 4;
    var label = match reps {
        1 => "single",
        2..=5 => "strength",
    };
}
```

```text
example.snx:3:17: error[SNX0404]: match is not exhaustive; add a '_' arm
3 |     var label = match reps {
  |                 ^^^^^^^^^^^^
1 error
```

An arm with a guard does not count toward coverage, because the guard can
be false.

## Option

Synx has no `null`. A value that can be absent has the type `Option<T>`,
which you can also write as `T?`. Its variants are `Some(value)` and
`None`. Both names are available without the `Option.` prefix:

```synx
int? find_pr(&const str lift) {
    match lift {
        "Squat" => Some(405),
        "Deadlift" => Some(495),
        _ => None,
    }
}

main() {
    for (lift in ["Squat", "Curl"]) {
        match find_pr(lift) {
            Some(pounds) => print("{lift}: {pounds} lb"),
            None => print("{lift}: no record"),
        }
    }
}
```

```text
Squat: 405 lb
Curl: no record
```

Synx has no `if let`. To act only on `Some`, use a `match` with a `_ => {}`
arm, or test with a method. `Option` has these common methods and
operators:

```synx
main() {
    int? found = Some(5);
    int? missing = None;
    print("{found.is_some()} {missing.is_none()}");
    print("{missing.unwrap_or(0)}");
    print("{missing ?? 1}");
    print("{found.unwrap()}");
    print("{missing.unwrap()}");
}
```

```text
true true
0
1
5
Synx panic: called Option.unwrap on None
```

`x ?? fallback` gives the value inside `x`, or `fallback` when `x` is `None`.
`unwrap` stops the program with a panic when the value is `None`. Use it
only when `None` is a bug. [Chapter 9](09-errors.md) covers panics,
`Result`, and the `?` operator.

## Matching through a reference

When the value in a `match` is a reference, the names in the patterns
become references to the parts of the value. A read-only reference gives
`&const` bindings. A writable reference gives `&` bindings, so the arm can
change the value:

```synx
enum Loot {
    Gold(int),
    Item { String name; int level; },
}

void upgrade(&Loot loot) {
    match loot {
        Loot.Gold(amount) => { *amount *= 2; }
        Loot.Item { level } => { *level += 1; }
    }
}

void show(&const Loot loot) {
    match loot {
        Loot.Gold(amount) => print("{amount} gold"),
        Loot.Item { name, level } => print("{name}, level {level}"),
    }
}

main() {
    var coins = Loot.Gold(50);
    var blades = Loot.Item { name: String.from("Mantis Blades"), level: 3 };
    upgrade(&coins);
    upgrade(&blades);
    show(coins);
    show(blades);
}
```

```text
100 gold
Mantis Blades, level 4
```

In `upgrade`, `amount` has the type `&int`. The arm writes through it with
`*amount`. An assignment is not an expression in Synx, so an arm that
assigns uses a block. The call `show(coins)` needs no `&const`, because a
`&const` parameter borrows its argument automatically. A match on a value,
without a reference, copies or moves the parts into the names. Chapters [7](07-ownership.md) and
[8](08-references.md) explain moves and references.

## Next

[Chapter 7](07-ownership.md) explains ownership: moves, copies, and when
values are dropped.
