# 12. Closures

A closure is a function without a name that can use local variables of the
code around it. A closure is a value. You can store it in a variable, pass
it to a function, and return it from a function.

## Closure syntax

A closure lists its parameters between `|` characters. The body follows.
The body is one expression or a block:

```synx
main() {
    var add = |int a, int b| a + b;
    var twice = |a, b| {
        int sum = a + b;
        sum * 2
    };
    var greet = || print("ready");
    print("{add(2, 3)} {twice(2, 3)}");
    greet();
}
```

```text
5 10
ready
```

The parameter types can be left out when the compiler can find them from
the use. `||` makes a closure with no parameters. The last expression in a
block body without a `;` is the result.

## Function types

The type `fn(int) -> int` is a function value that takes an `int` and
returns an `int`. `fn(int)` returns nothing. A parameter of a function type
accepts a closure and also a named function:

```synx
int apply(fn(int) -> int f, int x) {
    return f(x);
}

void each(&const Vec<int> items, fn(int) visit) {
    for (item in items) {
        visit(*item);
    }
}

int double_it(int x) {
    return x * 2;
}

main() {
    print("{apply(|x| x + 1, 5)}");
    print("{apply(double_it, 21)}");
    var reps = Vec<int>.new();
    reps.push(8);
    reps.push(6);
    each(&const reps, |int r| { print("{r} reps"); });
}
```

```text
6
42
8 reps
6 reps
```

## Captures

A closure captures the outer variables that its body uses. A Copy value,
such as an `int`, is copied into the closure. A change inside the closure
changes the copy only:

```synx
main() {
    int sets = 3;
    var add_set = || {
        sets += 1;
        sets
    };
    print("{add_set()} {add_set()} {sets}");
}
```

```text
4 5 3
```

A value that is not Copy, such as a `String`, moves into the closure. The
outer variable cannot be used after that:

```synx
main() {
    var song = String.from("Wonderwall");
    var play = || print(song);
    play();
    print(song);
}
```

```text
example.snx:5:11: error[SNX0501]: use of moved value `song`; it was moved earlier
5 |     print(song);
  |           ^^^^
1 error
```

## Capture lists

A capture list before the parameters selects how each variable is
captured. `&const name` borrows it for reading. `&name` borrows it for
writing. A plain `name` copies or moves it, as above:

```synx
main() {
    int total = 0;
    var bonus = Vec<int>.new();
    bonus.push(10);
    {
        var add = [&total, &const bonus] |int pounds| {
            total += pounds + bonus.len() as int;
        };
        add(135);
        add(185);
    }
    print("total {total}");
}
```

```text
total 322
```

A capture list must name every outer variable that the body uses:

```synx
main() {
    int reps = 5;
    int sets = 3;
    var volume = [&const reps] || reps * sets;
}
```

```text
example.snx:4:18: error[SNX0408]: captured variable 'sets' is not in the capture list
4 |     var volume = [&const reps] || reps * sets;
  |                  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^
1 error
```

A borrow capture follows the borrow rules of
[chapter 8](08-references.md). While the closure is in use, a writable
borrow blocks other uses of the variable. In the example above, the inner
block ends the closure, so `print` can read `total` after it.

## Call modes

A function type has one of three call modes:

| Type | Literal | A call can |
|------|---------|------------|
| `fn(A) -> R` | `\|A x\| body` | change captures; the closure can be called again |
| `fn const(A) -> R` | `const \|A x\| body` | only read captures; the closure can be called again |
| `fn once(A) -> R` | `once \|A x\| body` | move captures out; the closure is used up |

A `fn const` closure can be called through a shared reference. A `fn once`
closure can give away the values that it owns:

```synx
int run_twice(fn const() -> int f) {
    return f() + f();
}

String finish(fn once() -> String f) {
    return f();
}

main() {
    int plates = 4;
    var weight = const || 45 + plates * 45;
    print("{run_twice(weight)}");

    var title = String.from("Paint It Black");
    var take = [title] once || title;
    print(finish(take));
}
```

```text
450
Paint It Black
```

The body of a `const` closure cannot change a capture. A `once` closure
cannot be called a second time:

```synx
main() {
    var title = String.from("Paint It Black");
    var take = [title] once || title;
    String first = take();
    String second = take();
}
```

```text
example.snx:5:21: error[SNX0501]: use of moved value `take`; it was moved earlier
5 |     String second = take();
  |                     ^^^^^^
1 error
```

A `fn const` value can be used where a `fn` is expected. Named functions
work with all three modes. Other changes of mode are errors:

```synx
int read(fn const() -> int f) {
    return f();
}

main() {
    int base = 4;
    var plain = || base;
    print("{read(plain)}");
}
```

```text
example.snx:8:18: error[SNX0402]: type mismatch in argument: expected 'fn const() -> int', found 'fn() -> int'
8 |     print("{read(plain)}");
  |                  ^^^^^
1 error
```

## Function values move

All function values move, also when every capture is Copy. To call a
closure after you pass it to a function, pass a reference. A `&fn` parameter
can call a `fn` closure. A `&const fn const` parameter can call a
`fn const` closure:

```synx
int apply(&fn(int) -> int f, int x) {
    return f(x);
}

int peek(&const fn const(int) -> int f, int x) {
    return f(x);
}

main() {
    var triple = |int x| x * 3;
    print("{apply(&triple, 2)} {triple(3)}");
    var half = const |int x| x / 2;
    print("{peek(&const half, 10)} {half(8)}");
}
```

```text
6 9
5 4
```

## Returning closures

A function can return a closure. Write the function type as the return
type. Captured values move into the closure, so they live as long as it
does:

```synx
fn(int) -> int add_plates(int pairs) {
    return |int bar| bar + pairs * 90;
}

fn() -> int counter() {
    int count = 0;
    return || {
        count += 1;
        count
    };
}

main() {
    var two_pairs = add_plates(2);
    print("{two_pairs(45)} lb");
    var next = counter();
    print("{next()} {next()} {next()}");
}
```

```text
225 lb
1 2 3
```

A returned closure cannot borrow a local variable of the function, because
the variable is destroyed when the function returns:

```synx
fn() -> int counter() {
    int count = 0;
    return [&count] || {
        count += 1;
        count
    };
}

main() {
    var next = counter();
}
```

```text
example.snx:3:5: error[SNX0504]: `count` does not live past this scope; a reference is still in use
3 |     return [&count] || {
  |     ^^^^^^^^^^^^^^^^^^^^
example.snx:3:12: note: the borrow is here
3 |     return [&count] || {
  |            ^^^^^^^^^^^^^
example.snx:3:12: error[SNX0502]: this returns a reference to `count`, which does not live past the function
3 |     return [&count] || {
  |            ^^^^^^^^^^^^^
2 errors
```

## Storing closures

Function values can be stored in struct fields, tuples, arrays, `Vec`, and
`HashMap`. Call a field with `value.field(...)`:

```synx
struct Quickhack {
    &const str name;
    fn(int) -> int effect;
}

main() {
    Vec<Quickhack> deck = Vec.new();
    deck.push(Quickhack { name: "Overheat", effect: |int hp| hp - 40 });
    deck.push(Quickhack { name: "Short Circuit", effect: |int hp| hp / 2 });
    int health = 200;
    for (hack in &deck) {
        health = hack.effect(health);
        print("{hack.name}: {health} HP");
    }
}
```

```text
Overheat: 160 HP
Short Circuit: 80 HP
```

The loop uses `&deck` because a `fn` call needs writable access to the
function value. The [closures reference](../../reference/closures.md)
describes the rules for captures and lifetimes in more detail.

## Next

[Chapter 13](13-modules-and-files.md) covers modules, imports, and files.
