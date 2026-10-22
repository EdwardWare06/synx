# 9. Errors

Synx has two ways to report a failure. A panic stops the program. Use it for
bugs, such as an index out of range. A `Result` is a value that holds a
success or an error. Use it for failures that a caller can expect and
handle, such as bad input.

Synx has no exceptions. There is no `try`, no `catch`, and no unwinding of
the stack. A function that can fail says so in its return type.

## Panics

`panic` prints `Synx panic: ` and a message, then stops the program with
exit status 101:

```synx
void check_weight(int pounds) {
    if (pounds <= 0) {
        panic("weight must be positive");
    }
}

main() {
    check_weight(135);
    print("135 is valid");
    check_weight(0);
    print("this line does not run");
}
```

```text
135 is valid
Synx panic: weight must be positive
```

Some operations panic by themselves. An array index out of range, a division
by zero, and an integer overflow all cause a panic:

```synx
main() {
    int[3] sets = [5, 5, 3];
    int zero = 0;
    print("{sets[0] / zero}");
}
```

```text
Synx panic: division by zero
```

A panic does not run destructors, and no code can stop it. The operating
system frees the memory of the process.

## Result

`Result<T, E>` is an enum in the prelude. `Ok(value)` holds a success value
of type `T`. `Err(error)` holds an error of type `E`. Use `match` to handle
both cases:

```synx
Result<int, &const str> parse_reps(&const str text) {
    match text.parse_int() {
        Some(reps) => {
            if (reps <= 0) {
                return Err("reps must be positive");
            }
            return Ok(reps);
        }
        None => return Err("reps must be a number"),
    }
}

void show(&const str text) {
    match parse_reps(text) {
        Ok(reps) => print("{reps} reps"),
        Err(message) => print("error: {message}"),
    }
}

main() {
    show("8");
    show("-2");
    show("eight");
}
```

```text
8 reps
error: reps must be positive
error: reps must be a number
```

`parse_int` is a method of `&const str`. It returns an `int?`, which is
another name for `Option<int>`. Chapter 6 covers `Option`.

`is_ok()` and `is_err()` test a `Result` without a `match`:

```synx
Result<int, &const str> divide(int a, int b) {
    if (b == 0) {
        return Err("division by zero");
    }
    return Ok(a / b);
}

main() {
    var good = divide(10, 2);
    var bad = divide(10, 0);
    print("{good.is_ok()} {bad.is_err()}");
}
```

```text
true true
```

## The ? operator

A function that calls other functions that return `Result` often passes each
error on to its caller. The postfix `?` operator does this. When the value is
`Ok`, `?` gives the success value. When the value is `Err`, `?` returns the
error from the current function at once:

```synx
Result<int, &const str> parse_reps(&const str text) {
    match text.parse_int() {
        Some(reps) => return Ok(reps),
        None => return Err("not a number"),
    }
}

Result<int, &const str> total_reps(&const str first, &const str second) {
    int a = parse_reps(first)?;
    int b = parse_reps(second)?;
    return Ok(a + b);
}

void show(Result<int, &const str> result) {
    match result {
        Ok(total) => print("total {total}"),
        Err(message) => print("error: {message}"),
    }
}

main() {
    show(total_reps("8", "6"));
    show(total_reps("8", "six"));
}
```

```text
total 14
error: not a number
```

The function that uses `?` must return a `Result`. The error type must be the
same type. Synx does not convert one error type to another:

```synx
enum ReadError { Missing }
enum LoadError { Broken }

Result<int, ReadError> read() {
    return Err(ReadError.Missing);
}

Result<int, LoadError> load() {
    int value = read()?;
    return Ok(value);
}

main() {
    var result = load();
}
```

```text
example.snx:9:17: error: '?' error type mismatch: this Result has error type 'ReadError' but the function returns 'Result<int, LoadError>'
9 |     int value = read()?;
  |                 ^^^^^^^
1 error
```

To change the error type, use `match` and return a new `Err`.

## ? with Option

`?` also works on an `Option` in a function that returns an `Option`. On
`None`, the function returns `None`:

```synx
int? half(int n) {
    if (n % 2 != 0) {
        return None;
    }
    return Some(n / 2);
}

int? quarter(int n) {
    int h = half(n)?;
    return half(h);
}

main() {
    print("{quarter(12) ?? -1}");
    print("{quarter(6) ?? -1}");
}
```

```text
3
-1
```

`a ?? b` gives the value in `a` when `a` is `Some`, and `b` when `a` is
`None`. Chapter 6 shows more of the `Option` methods.

## unwrap and expect

An `Option` has the methods `unwrap()` and `expect(message)`. Each one gives
the value in `Some`. On `None`, `unwrap()` panics with a fixed message and
`expect` panics with your message:

```synx
main() {
    int? best = Some(315);
    print("best squat: {best.unwrap()} lb");
    int? goal = None;
    int value = goal.expect("no goal is set");
    print("{value}");
}
```

```text
best squat: 315 lb
Synx panic: no goal is set
```

Use `expect` when `None` would be a bug in your program. Use `match`, `??`,
or `unwrap_or(fallback)` when `None` is a normal case.

`Result` has no `unwrap` method. Handle a `Result` with `match` or `?`.

## An error enum

A program usually has an enum for its errors. The variants can carry data.
A `Display` implementation lets you put the error in a string. Chapter 11
explains traits such as `Display`.

```synx
enum LootError {
    InventoryFull,
    TooHeavy(int),
    Locked(&const str),
}

impl Display for LootError {
    String to_string(&const self) {
        match self {
            InventoryFull => return String.from("inventory is full"),
            TooHeavy(weight) => return "item weighs {weight} kg",
            Locked(name) => return "{name} is locked",
        }
    }
}

Result<int, LootError> pick_up(int weight, int free_slots) {
    if (free_slots == 0) {
        return Err(LootError.InventoryFull);
    }
    if (weight > 40) {
        return Err(LootError.TooHeavy(weight));
    }
    return Ok(free_slots - 1);
}

main() {
    match pick_up(12, 3) {
        Ok(left) => print("picked up, {left} slots left"),
        Err(error) => print("cannot pick up: {error}"),
    }
    match pick_up(55, 3) {
        Ok(left) => print("picked up, {left} slots left"),
        Err(error) => print("cannot pick up: {error}"),
    }
    var locked = LootError.Locked("Arasaka chest");
    print("{locked}");
}
```

```text
picked up, 2 slots left
cannot pick up: item weighs 55 kg
Arasaka chest is locked
```

A `match` can also look inside the error. A pattern such as
`Err(LootError.TooHeavy(weight))` matches one variant only.

## Errors in main

`main` cannot return a `Result`. It returns `int` or nothing. A common form
puts the work in a function that returns a `Result`. `main` then turns the
error into a message and an exit status:

```synx
enum ConfigError {
    MissingValue,
    BadNumber,
}

Result<int, ConfigError> read_sets(&const str text) {
    if (text.len() == 0) {
        return Err(ConfigError.MissingValue);
    }
    match text.parse_int() {
        Some(sets) => return Ok(sets),
        None => return Err(ConfigError.BadNumber),
    }
}

Result<int, ConfigError> run() {
    int sets = read_sets("5")?;
    int extra = read_sets("x")?;
    return Ok(sets + extra);
}

int main() {
    match run() {
        Ok(total) => {
            print("{total} sets");
            return 0;
        }
        Err(ConfigError.MissingValue) => print("error: a value is missing"),
        Err(ConfigError.BadNumber) => print("error: a value is not a number"),
    }
    return 1;
}
```

```text
error: a value is not a number
```

The exit status of this program is 1. The `?` operator cannot be used in
`main` itself, because `main` does not return a `Result`.

## Next

[Chapter 10](10-strings-and-collections.md) covers strings, vectors, and
hash maps.
