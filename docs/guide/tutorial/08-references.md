# 8. References

A reference lets code use a value without taking ownership of it. This is
called borrowing. Synx has two kinds of reference:

| Type | Access | How many at one time |
|------|--------|----------------------|
| `&const T` | Read only | Any number |
| `&T` | Read and write | One, and no `&const` references at the same time |

The compiler checks these rules. A program that breaks them does not compile.

## Shared references

A parameter of type `&const T` borrows the argument. The caller keeps
ownership and can use the value after the call:

```synx
void show(&const String title, int track) {
    print("{track}. {title}");
}

main() {
    var title = String.from("The Summoning");
    show(title, 1);
    print(title);
}
```

```text
1. The Summoning
The Summoning
```

The call `show(title, 1)` borrows `title` automatically. You can also write
`show(&const title, 1)`.

A `&const str` parameter accepts a string literal and also a `String`. A
`&const T[]` parameter, called a slice, accepts an array and a `Vec<T>`:

```synx
int total(&const int[] weights) {
    int sum = 0;
    for (w in weights) {
        sum += *w;
    }
    return sum;
}

main() {
    int[3] warmup = [45, 95, 135];
    Vec<int> work = Vec.new();
    work.push(225);
    work.push(245);
    print("{total(warmup)} {total(work)}");
}
```

```text
275 470
```

The loop over a slice gives a reference to each element, so the loop reads
each value with `*w`.

## Writable references

A parameter of type `&T` can change the value. The caller must write `&` at
the call. This shows at the call site that the argument can change:

```synx
struct Lift {
    int pounds;
    int reps;
}

void add_weight(&Lift lift, int amount) {
    lift.pounds += amount;
}

main() {
    var squat = Lift { pounds: 225, reps: 5 };
    add_weight(&squat, 10);
    print("{squat.pounds}");
}
```

```text
235
```

Fields are reached through a reference with `.`, as in `lift.pounds`. For a
reference to a number or other plain value, use `*` to read or write the
value:

```synx
void double_it(&int value) {
    *value = *value * 2;
}

main() {
    int reps = 6;
    double_it(&reps);
    print("{reps}");
}
```

```text
12
```

## Methods

A method states how it uses its receiver: `&const self` reads, `&self`
writes, and `self` takes ownership. The call borrows the receiver
automatically, so a call is always written `value.method()`:

```synx
struct Counter {
    int count;

    void add(&self) {
        self.count += 1;
    }

    int get(&const self) {
        return self.count;
    }
}

main() {
    var c = Counter { count: 0 };
    c.add();
    c.add();
    print("{c.get()}");
}
```

```text
2
```

## The borrow rules

While a `&T` reference is in use, no other reference to the same value can
be made:

```synx
main() {
    int count = 3;
    var a = &count;
    var b = &count;
    *a += 1;
    *b += 1;
    print("{count}");
}
```

```text
example.snx:4:13: error[SNX0501]: cannot borrow `count` as writable while it is already borrowed
4 |     var b = &count;
  |             ^^^^^^
example.snx:3:13: note: the other borrow is here and is still in use
3 |     var a = &count;
  |             ^^^^^^
1 error
```

A borrow ends after its last use. It does not need to reach the end of the
block. The next program compiles, because `a` is not used after `b` is made:

```synx
main() {
    int count = 3;
    var a = &count;
    *a += 1;
    var b = &count;
    *b += 1;
    print("{count}");
}
```

```text
5
```

The same rule stops a change to a collection while code reads from it:

```synx
main() {
    Vec<int> sets = Vec.new();
    var first = sets.get(0);
    sets.push(135);
    print("{first.is_some()}");
}
```

```text
example.snx:4:5: error[SNX0501]: cannot borrow `sets` as writable while it is already borrowed
4 |     sets.push(135);
  |     ^^^^
example.snx:3:17: note: the other borrow is here and is still in use
3 |     var first = sets.get(0);
  |                 ^^^^
1 error
```

`push` can move the elements to a larger block of memory. A reference
returned by `get` would then point to freed memory.

## References must not outlive their values

A function cannot return a reference to one of its own local variables. The
local is destroyed when the function returns:

```synx
&const int pick() {
    int value = 5;
    return &const value;
}

main() {
    print("{*pick()}");
}
```

```text
example.snx:3:5: error[SNX0504]: `value` does not live past this scope; a reference is still in use
3 |     return &const value;
  |     ^^^^^^^^^^^^^^^^^^^
example.snx:3:12: note: the borrow is here
3 |     return &const value;
  |            ^^^^^^^^^^^^
example.snx:3:12: error[SNX0502]: this returns a reference to `value`, which does not live past the function
3 |     return &const value;
  |            ^^^^^^^^^^^^
2 errors
```

A function can return a reference that it received. The result then borrows
from the arguments:

```synx
&const int larger(&const int a, &const int b) {
    if (*a > *b) {
        return a;
    }
    return b;
}

main() {
    int x = 3;
    int y = 8;
    print("{*larger(x, y)}");
}
```

```text
8
```

Synx has no lifetime annotations. The compiler finds which arguments a
returned reference can come from by reading the body of the function.

## Next

[Chapter 9](09-errors.md) shows how a program reports and handles errors.
