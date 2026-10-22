# 1. First steps

Synx is a compiled language. The compiler reads `.snx` source files and makes
a native program. There is no interpreter and no virtual machine. A Synx
program manages its memory without a garbage collector.

If you know Java or Python, much of the syntax will be familiar. Types are
written before names, as in Java and C. Blocks use braces and statements end
with a semicolon. The large difference is memory: each value has one owner,
and the compiler checks every use of a value before the program runs.
Chapters 7 and 8 explain this.

Before you start, install Synx and run the first program in
[Getting started](../getting-started.md).

## Running the examples

Put each example in a file, for example `example.snx`, and run it:

```text
synx run example.snx
```

Every example in this tutorial is a complete program. The output is shown
after each example.

## Printing

```synx
main() {
    print("Squat day");
    print("Sets: 5");
}
```

```text
Squat day
Sets: 5
```

`print` writes its argument and a line break. The program starts in `main`.

## Comments

```synx
// A line comment runs to the end of the line.

/* A block comment can span lines.
   /* Block comments nest. */ */

/// A documentation comment describes the item that follows it.
main() {
    print("comments");  // A comment can follow code.
}
```

## Values in text

A string literal can hold expressions in braces. Each expression is
converted to text:

```synx
main() {
    int sets = 5;
    int reps = 5;
    print("{sets} x {reps} = {sets * reps} reps");
}
```

```text
5 x 5 = 25 reps
```

To write a brace character, use `\{` and `\}`:

```synx
main() {
    print("set notation: \{1, 2, 3\}");
}
```

```text
set notation: {1, 2, 3}
```

## Numbers

Synx has the usual operators. Integer division rounds toward zero, and `%`
gives the remainder:

```synx
main() {
    print("{17 / 5}");
    print("{17 % 5}");
    print("{17.0 / 5.0}");
    print("{2 + 3 * 4}");
}
```

```text
3
2
3.4
14
```

A whole number such as `17` is an `int`. A number with a decimal point is a
`double`. The two types do not mix without a conversion. `17 / 5.0` is an
error. Chapter 2 shows how to convert.

## Exit status

`main` can return an `int`. The value is the exit status of the program:

```synx
int main() {
    print("checks failed");
    return 1;
}
```

When `main` has no return type, the exit status is 0.

## Next

[Chapter 2](02-variables-and-types.md) introduces variables and the basic
types.
