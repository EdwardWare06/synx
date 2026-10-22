# 4. Functions

## Declaring a function

```synx
int volume(int sets, int reps, int pounds) {
    return sets * reps * pounds;
}

main() {
    print("{volume(5, 5, 225)} lb");
}
```

```text
5625 lb
```

The return type comes first, as in Java. There is no `fn` or `def`
keyword. Each parameter is a type and a name. A function with no result has
the return type `void`.

A function can come before or after the code that calls it. The order of
declarations in a file does not matter.

## The last expression

A function body is a block. When the last expression of the block has no
semicolon, it is the return value:

```synx
int square(int n) {
    n * n
}

double average(int total, int count) {
    total as double / count as double
}

main() {
    print("{square(12)}");
    print("{average(7, 2)}");
}
```

```text
144
3.5
```

A semicolon after the last expression discards its value. The function then
has no value to return:

```synx
int square(int n) {
    n * n;
}

main() {
    print("{square(3)}");
}
```

```text
example.snx:3:1: error: missing return value: function returns 'int'
3 | }
  | ^
1 error
```

`return` leaves the function at once. Use it for an early exit:

```synx
int sign(int x) {
    if (x < 0) {
        return -1;
    }
    if (x == 0) {
        return 0;
    }
    1
}

main() {
    print("{sign(-4)} {sign(0)} {sign(7)}");
}
```

```text
-1 0 1
```

In a `void` function, `return;` leaves the function without a value.

The semicolon after a `return` is optional. A `return` never finishes, so
nothing comes after it either way. Use whichever form reads better to you:

```synx
int square(int n) {
    return n * n
}

int clamp(int n) {
    if (n > 100) {
        return 100
    }
    return n;
}

main() {
    print("{square(9)} {clamp(250)} {clamp(42)}");
}
```

```text
81 100 42
```

## Inferred return types

You can omit the return type. The compiler then finds it from the body:

```synx
half(int n) {
    n / 2
}

greet(&const str name) {
    print("Hello, {name}");
}

main() {
    greet("Ada");
    print("{half(9)}");
}
```

```text
Hello, Ada
4
```

`half` returns `int`. `greet` has no result, so it returns `void`. All
`return` statements and the last expression must agree on one type:

```synx
pick(bool heavy) {
    if (heavy) {
        return 315;
    }
    return "light";
}

main() {
    print("{pick(true)}");
}
```

```text
example.snx:5:12: error[SNX0402]: type mismatch in return value: expected '{integer}', found '&const str'
5 |     return "light";
  |            ^^^^^^^
1 error
```

Three kinds of function must write the return type: a function that calls
itself, a `pub` function, and a function declaration without a body.
[Chapter 13](13-modules-and-files.md) covers `pub`.

## Parameters

An argument of a type such as `int` or `double` is copied into the
parameter. The function can assign to its parameter. The change does not
affect the caller:

```synx
void add_warmup(int sets) {
    sets += 2;
    print("inside: {sets}");
}

main() {
    int sets = 3;
    add_warmup(sets);
    print("outside: {sets}");
}
```

```text
inside: 5
outside: 3
```

The `&const str` parameter of `greet` above is a read-only reference.
Chapters [7](07-ownership.md) and [8](08-references.md) explain how values of
other types are moved or borrowed into parameters.

Each call must give one argument for each parameter, of the correct type.
Synx has no default arguments and no overloading. Two functions with the
same name in one module are an error:

```synx
int area(int side) {
    return side * side;
}

int area(int width, int height) {
    return width * height;
}

main() {
    print("{area(3)}");
}
```

```text
example.snx:5:5: error[SNX0302]: 'area' is already defined
5 | int area(int width, int height) {
  |     ^^^^
example.snx:1:5: note: previous definition is here
1 | int area(int side) {
  |     ^^^^
1 error
```

## Returning more than one value

A function can return a tuple. Read its fields with `.0` and `.1`:

```synx
(int, int) min_max(int a, int b) {
    if (a < b) { (a, b) } else { (b, a) }
}

main() {
    var range = min_max(9, 3);
    print("{range.0} to {range.1}");
}
```

```text
3 to 9
```

A declaration such as `var (low, high) = min_max(9, 3);` is not valid Synx.
[Chapter 6](06-enums-and-match.md) shows how `match` takes a tuple apart.

## Recursion

A function can call itself. It must then write its return type:

```synx
long factorial(int n) {
    if (n <= 1) { 1L } else { n as long * factorial(n - 1) }
}

main() {
    print("{factorial(20)}");
}
```

```text
2432902008176640000
```

Without the return type, the compiler stops with an error:

```synx
factorial(int n) {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}

main() {
    print("{factorial(5)}");
}
```

```text
example.snx:5:16: error[SNX0405]: function 'factorial' calls itself, so it must write its return type
5 |     return n * factorial(n - 1);
  |                ^^^^^^^^^
example.snx:1:1: note: add the return type here
1 | factorial(int n) {
  | ^^^^^^^^^
1 error
```

## Functions as values

A function name is a value. The type `fn(int) -> int` describes a function
that takes an `int` and returns an `int`:

```synx
int double_it(int x) {
    x * 2
}

int apply_twice(fn(int) -> int f, int x) {
    f(f(x))
}

main() {
    print("{apply_twice(double_it, 5)}");
    var g = double_it;
    print("{g(21)}");
}
```

```text
20
42
```

[Chapter 12](12-closures.md) covers closures and the other function types.

## main

`main` has one of three forms:

```text
main() { ... }
void main() { ... }
int main() { ... }
```

The first two forms are the same. They end the program with exit status 0.
With `int main()`, the return value is the exit status. `main` has no
parameters. To read the command line, call `std.env.args()`. It returns the
arguments as a `Vec<String>`, with the program name first:

```synx
import std.env;

main() {
    var args = env.args();
    print("{args.len()} argument(s)");
}
```

```text
1 argument(s)
```

The program in this example ran without arguments, so the list holds only the
program name.

## Next

[Chapter 5](05-structs-and-methods.md) introduces structs and methods.
