# 3. Control flow

## if

```synx
main() {
    int weight = 275;
    if (weight >= 315) {
        print("three plates a side");
    } else if (weight >= 225) {
        print("two plates a side");
    } else {
        print("one plate a side");
    }
}
```

```text
two plates a side
```

The parentheses around the condition are required. The braces are also
required, even when a branch has one statement.

The condition must have the type `bool`. Synx does not treat a number or a
reference as true or false:

```synx
main() {
    int reps = 5;
    if (reps) {
        print("done");
    }
}
```

```text
example.snx:3:9: error[SNX0402]: type mismatch in if condition: expected 'bool', found 'int'
3 |     if (reps) {
  |         ^^^^
1 error
```

Write `if (reps > 0)` instead.

## if as a value

`if` is an expression. The last expression in a block, written without a
semicolon, is the value of the block. The `if` gives the value of the branch
that runs:

```synx
main() {
    int x = -4;
    var sign = if (x < 0) { "negative" } else if (x == 0) { "zero" } else { "positive" };
    print(sign);
}
```

```text
negative
```

An `if` that gives a value must have an `else` branch. All branches must
give the same type. Synx has no `? :` operator. Use `if` for that purpose.

## while

`while` tests its condition before each pass through the body:

```synx
main() {
    int bar = 135;
    while (bar < 405) {
        print("warm-up: {bar} lb");
        bar += 90;
    }
    print("work set: {bar} lb");
}
```

```text
warm-up: 135 lb
warm-up: 225 lb
warm-up: 315 lb
work set: 405 lb
```

## loop

`loop` repeats its body until a `break` stops it:

```synx
main() {
    int n = 1;
    loop {
        if (n * n > 50) {
            break;
        }
        n += 1;
    }
    print("{n} squared is the first square over 50");
}
```

```text
8 squared is the first square over 50
```

In Synx, `break` does not carry a value, and `loop` does not give a value.
`break 5;` is a syntax error. To get a result out of a loop, assign it to a
variable that was declared before the loop, as the example does with `n`.

## for and ranges

`for` runs its body once for each value in a sequence. The parentheses are
required. A range is the simplest sequence:

```synx
main() {
    for (i in 0..3) {
        print("set {i}");
    }
    for (i in 1..=3) {
        print("round {i}");
    }
}
```

```text
set 0
set 1
set 2
round 1
round 2
round 3
```

`a..b` starts at `a` and stops before `b`. `a..=b` includes `b`. The bounds
can be any integer expressions. When the range is empty, for example `5..2`,
the body does not run.

Array indexes have the type `usize`. A range of literals takes the type that
the body needs, so `i` below can index the array:

```synx
main() {
    int[4] plates = [45, 35, 25, 10];
    for (i in 0..4) {
        print("plate {i}: {plates[i]} lb");
    }
}
```

```text
plate 0: 45 lb
plate 1: 35 lb
plate 2: 25 lb
plate 3: 10 lb
```

An `int` variable cannot index an array. Convert it with `as usize`, for
example `plates[k as usize]`.

## for over arrays

A `for` loop can go through the elements of an array:

```synx
main() {
    int[3] reps = [8, 6, 4];
    int total = 0;
    for (r in reps) {
        total += r;
    }
    print("{total} reps");
}
```

```text
18 reps
```

The loop copies each element into `r`. This form is permitted only when the
element type can be copied, such as `int`. For other element types, loop
over a reference to the array.

`for (x in &const a)` gives a read-only reference to each element.
`for (x in &a)` gives a writable reference. Use `*` to read or write the
value through the reference:

```synx
main() {
    int[3] weights = [135, 155, 175];
    for (w in &weights) {
        *w += 10;
    }
    int total = 0;
    for (w in &const weights) {
        total += *w;
        print("{w}");
    }
    print("total {total}");
}
```

```text
145
165
185
total 495
```

Interpolation prints the value that a reference points to, so `{w}` needs no
`*`. Chapters [7](07-ownership.md) and [8](08-references.md) explain copies,
moves, and references. [Chapter 10](10-strings-and-collections.md) shows
`for` over a `Vec`.

## break and continue

`continue` goes to the next pass of the loop. `break` leaves the loop:

```synx
main() {
    int[6] costs = [6, 14, 3, 9, 1, 2];
    int ram = 10;
    for (cost in costs) {
        if (cost > ram) {
            continue;
        }
        print("upload quickhack: {cost} RAM");
        ram -= cost;
        if (ram == 0) {
            break;
        }
    }
    print("{ram} RAM left");
}
```

```text
upload quickhack: 6 RAM
upload quickhack: 3 RAM
upload quickhack: 1 RAM
0 RAM left
```

`break` and `continue` act on the innermost loop. Synx has no loop labels:

```synx
main() {
    for (row in 0..3) {
        for (col in 0..3) {
            if (col > row) {
                break;
            }
            print("{row},{col}");
        }
    }
}
```

```text
0,0
1,0
1,1
2,0
2,1
2,2
```

To leave an outer loop, put the loops in a function and use `return`.

## Next

[Chapter 4](04-functions.md) covers functions.
