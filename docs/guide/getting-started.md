# Getting started

This page builds and runs a first program, shows how the compiler reports
errors, and splits a program into two files. It assumes that `synx` is
installed and on `PATH`. See [Installing Synx](install.md).

## A first program

Make a file named `hello.snx`:

```synx
main() {
    print("Hello, world!");
}
```

Build and run it:

```text
> synx run hello.snx
Hello, world!
```

`main` is the program entry point. It has no return type here, so the program
exits with status 0. `print` writes one line to standard output. It is
available in every file without an import.

`synx run` builds in a private cache and leaves the source folder clear.
It reuses the executable when the program has not changed. Use `synx build`
to write an executable for distribution:

```text
> synx build hello.snx
Built hello.exe
```

The build writes `hello.exe` on Windows or `hello` on Linux and macOS. The
executable includes the Synx runtime. Copy this one file to another computer
with the same supported OS and architecture.

Use `-o` to choose another output path. The folder must exist:

```text
> synx build hello.snx -o bin/hello.exe
Built bin/hello.exe
```

## Exit status

When `main` returns `int`, the value is the exit status of the program.
`synx run` exits with the status of the program:

```synx
int main() {
    return 7;
}
```

```text
> synx run status.snx
> echo %ERRORLEVEL%
7
```

On Linux, use `echo $?`.

## Compile errors

The compiler checks types, names, and ownership before it makes a program.
Each error has a code, a location, and the line of source:

```synx
main() {
    int count = "three";
    print("{count}");
}
```

```text
> synx build bad.snx
bad.snx:2:17: error[SNX0402]: type mismatch in variable initializer: expected 'int', found '&const str'
2 |     int count = "three";
  |                 ^^^^^^^
1 error
```

The location is `file:line:column`. When there are errors, no program is
made and `synx` exits with status 1. `synx check` runs the same checks
without a build. It prints `bad.snx: ok` when there are no errors.

## Panics

A panic stops a program that finds a bug at run time, for example an index
out of bounds or an integer overflow:

```synx
int main() {
    int[3] values = [1, 2, 3];
    usize i = 5;
    print("{values[i]}");
    return 0;
}
```

```text
> synx run index.snx
Synx panic: index out of bounds
```

The message goes to standard error and the exit status is 101. Expected
failures, such as a missing file, are values of type `Result`. The
[tutorial](tutorial/index.md) explains both.

Integer overflow causes a panic in a normal build. With `--release`,
arithmetic wraps and the overflow checks are removed:

```text
> synx build overflow.snx --release
```

## A program in two files

A file is a module. A program can import other modules from the folder of
its main file. Items marked `pub` are visible to other modules.

`convert.snx`:

```synx
/// Converts between kilograms and pounds.

pub const double POUNDS_PER_KILOGRAM = 2.20462;

pub double to_pounds(double kilograms) {
    return kilograms * POUNDS_PER_KILOGRAM;
}

pub double to_kilograms(double pounds) {
    return pounds / POUNDS_PER_KILOGRAM;
}
```

`main.snx`:

```synx
import convert;
import std.io.input;

int main() {
    print("Weight in pounds:");
    var line = input();
    match line.trim().as_str().parse_int() {
        Some(pounds) => {
            var kilograms = convert.to_kilograms(pounds as double);
            print("{pounds} lb is {kilograms} kg");
            return 0;
        }
        None => {
            print("not a whole number: {line}");
            return 1;
        }
    }
}
```

Build the main file. The compiler finds `convert.snx` from the import:

```text
> synx run main.snx
Weight in pounds:
225
225 lb is 102.05840462301894 kg
```

`import convert;` makes the module available as `convert`. `import
std.io.input;` imports one function from the standard library, so the call is
`input()`. The `std` modules are part of the installation.

## Next

- The [tutorial](tutorial/index.md) teaches the language from the start.
- The [command reference](commands.md) lists every `synx` command and option.
- The [standard library reference](../library/index.md) describes each module.
