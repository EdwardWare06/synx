# 2. Variables and types

## Declaring variables

A declaration gives a type, a name, and usually a value:

```synx
main() {
    int reps = 8;
    double weight = 185.5;
    bool warmup = false;
    print("{reps} reps at {weight} lb, warmup: {warmup}");
}
```

```text
8 reps at 185.5 lb, warmup: false
```

`var` lets the compiler find the type from the value:

```synx
main() {
    var reps = 8;          // int
    var weight = 185.5;    // double
    var name = "Deadlift"; // &const str
    print("{name}: {reps} x {weight}");
}
```

```text
Deadlift: 8 x 185.5
```

The type is fixed when the variable is declared. A later assignment must have
the same type.

Variables can be changed by default. `const` makes a variable that cannot be
assigned again:

```synx
main() {
    var total = 0;
    total = total + 135;
    total += 90;
    const limit = 405;
    print("{total} of {limit}");
}
```

```text
225 of 405
```

A variable can be declared without a value. The compiler then checks that
every path assigns it before it is read:

```synx
main() {
    int sets;
    bool heavy = true;
    if (heavy) {
        sets = 3;
    } else {
        sets = 5;
    }
    print("{sets} sets");
}
```

```text
3 sets
```

## Basic types

| Type | Values |
|------|--------|
| `bool` | `true`, `false` |
| `byte` | Unsigned 8-bit integer |
| `short` | Signed 16-bit integer |
| `int` | Signed 32-bit integer |
| `long` | Signed 64-bit integer |
| `uint`, `ulong` | Unsigned 32-bit and 64-bit integers |
| `usize` | Unsigned integer the size of a pointer, used for lengths and indexes |
| `float`, `double` | 32-bit and 64-bit floating point |
| `char` | One Unicode character, such as `'a'` or `'\u{e9}'` |

Integer literals can use `_` as a separator, and the prefixes `0x` and `0b`.
The suffix `L` makes a `long` and the suffix `f` makes a `float`:

```synx
main() {
    long distance = 1_500_000L;
    int mask = 0xFF;
    float ratio = 0.5f;
    char grade = 'A';
    print("{distance} {mask} {ratio} {grade}");
}
```

```text
1500000 255 0.5 A
```

## Conversions

Synx does not convert between number types by itself. Use `as`:

```synx
main() {
    int reps = 7;
    double weight = 202.5;
    double volume = reps as double * weight;
    int rounded = volume as int;
    print("{volume} {rounded}");
}
```

```text
1417.5 1417
```

A conversion from a floating point type to an integer drops the fraction. A
conversion to a smaller integer type keeps the low bits.

## Overflow

Integer arithmetic is checked. A result that does not fit the type stops the
program with a panic:

```synx
main() {
    byte b = 250;
    b += 10;
    print("{b}");
}
```

```text
Synx panic: integer overflow
```

A program built with `synx build --release` skips this check. Arithmetic then
wraps, and the program above prints `4`. Use the default build while you
develop.

## Strings

A string literal such as `"Deadlift"` has the type `&const str`. It is text
that the program can read but not change. An interpolated literal such as
`"{reps} reps"` makes a new `String`, which the program owns and can change.
[Chapter 10](10-strings-and-collections.md) covers both types.

## Arrays and tuples

An array has a fixed number of elements of one type. The type `int[3]` is an
array of three `int` values. Indexes start at 0, and an index out of range
causes a panic:

```synx
main() {
    int[4] sets = [135, 185, 225, 245];
    int heaviest = sets[3];
    sets[0] = 155;
    print("{sets[0]} to {heaviest}");
}
```

```text
155 to 245
```

A tuple groups a fixed number of values of different types. Its fields are
`.0`, `.1`, and so on:

```synx
main() {
    (&const str, int) lift = ("Bench", 225);
    print("{lift.0}: {lift.1} lb");
}
```

```text
Bench: 225 lb
```

## Next

[Chapter 3](03-control-flow.md) covers `if`, loops, and ranges.
