# The synx command

```text
synx <command> [arguments]
```

`synx` with no command prints a usage summary and exits with status 1. An
unknown command prints an error and the usage summary and exits with status 1.

| Command | Purpose |
|---------|---------|
| [build](#build) | Compile a program to a native executable. |
| [run](#run) | Build a program and run it. |
| [check](#check) | Check a program without building it. |
| [version](#version) | Print the version and host. |
| [about](#about) | Describe this build of the compiler. |
| [lex, parse, modules, ir](#inspection-commands) | Print the output of one compiler stage. |

## build

```text
synx build <file.snx> [options]
```

Compiles the program whose `main` is in *file.snx* and writes a native
executable. Modules imported by the program are found relative to the folder
of *file.snx*. The `std` modules are found in the installation.

The default output is the input path with the extension removed, plus `.exe`
on Windows: `src/game.snx` builds `src/game.exe` or `src/game`. On success,
`synx` prints `Built <path>` and exits with status 0. On a compile error, it
prints the diagnostics and the error count to standard error and exits with
status 1.

The build also writes the runtime library (`synx_rt.dll` on Windows,
`libsynx_rt.so` on Linux, `libsynx_rt.dylib` on macOS) to the folder of the executable. The file is not
written again if an identical copy is already there.

Options can come before or after the file name:

| Option | Effect |
|--------|--------|
| `-o <path>` | Write the executable to *path*. The folder must exist. |
| `--release` | Remove integer overflow checks. Addition, subtraction, multiplication, and negation wrap. Shift counts are masked to the operand width. Division by zero still panics. Bounds checks stay. |
| `--keep-obj` | Keep the object file (`<output>.obj` on Windows, `<output>.o` elsewhere). |
| `--no-regalloc` | Keep all local values on the stack. This option is for compiler testing. |
| `--linker=builtin` | Link with the built-in linker. This is the default. |
| `--linker=system` | Link with the system linker: `link` on Windows, `cc` elsewhere. The linker must be on `PATH`. |
| `-v`, `--verbose` | Print the number of functions, the code size, and the link step to standard error. |

Example:

```text
> synx build hello.snx -o out/hello.exe -v
Dendrite IR: 1 functions
Axon code generator: x86_64-windows, 207 bytes of code
link: built-in linker
Built out/hello.exe
```

## run

```text
synx run <file.snx> [options]
```

Builds the program like `synx build`, with the same options, and then runs
it. The executable and the runtime library stay on disk. The program inherits
standard input, standard output, and standard error. It receives no
command-line arguments.

`synx run` exits with the exit status of the program. If the build fails, it
exits with status 1 and does not run anything.

## check

```text
synx check <file.snx>
```

Runs the checks of a build: names, types, ownership, and borrows. It does
not generate code. On success, it prints `<file.snx>: ok` and exits with
status 0. On errors, it prints the diagnostics and exits with status 1.

## version

```text
synx version
synx --version
```

Prints the version and the host operating system, for example
`Synx 0.1.0 (windows)`. The host is `windows`, `linux`, or `macos`.

## about

```text
synx about
```

Prints a short description of the compiler build: its target, its parts, and
its origins.

## Inspection commands

These commands print the internal form of a program at one stage of the
compiler. They are for compiler work and for learning how the compiler
works. Their output format can change between releases. Each command exits
with status 1 if the stage reports an error.

| Command | Output |
|---------|--------|
| `synx lex <file>` | One token per line, with byte offsets. |
| `synx parse <file>` | The syntax tree. `--strict` adds spans and every field. |
| `synx modules <file>` | Declarations and the names that each module can see. |
| `synx modules <file> --signatures` | Also resolved function and type signatures. |
| `synx modules <file> --check` | Also the types found by the checker. |
| `synx modules <file> --lower` | Also the Dendrite IR before borrow checking. |
| `synx modules <file> --borrow` | Also the IR after borrow checking and drop elaboration. |
| `synx modules <file> --async` | Also the IR after async functions become state machines. |
| `synx modules <file> --machine` | Also type layouts and register assignments. |
| `synx ir <file>` | The Dendrite IR of the program. |

Example:

```text
> synx ir status.snx
fn main() -> int {
  let _0: int  // return
  bb0:
    _0 = const 7_int
    goto bb1
  bb1:
    return
}
```

## Exit status

| Status | Meaning |
|--------|---------|
| 0 | Success. |
| 1 | Usage error, unreadable file, compile error, or link error. |
| 2 | `check` and the inspection commands only: internal error, the built-in prelude does not compile. |

`synx run` returns the status of the program instead, after a successful
build. A Synx program that panics exits with status 101.

## Environment variables

The compiler finds the standard library and the runtime from its own
location. These variables override that search. They are for development
builds and tests. An installed compiler needs none of them.

| Variable | Effect |
|----------|--------|
| `SYNX_STD_PATH` | Folder that holds the `std` modules. |
| `SYNX_RUNTIME_SHARED` | Path to the runtime library that the built-in linker uses and copies next to the program. |
| `SYNX_RUNTIME_LIB` | Path to the static runtime library (`synx_rt.lib` or `libsynx_rt.a`). When it is set, the default linker is the system linker. |
| `SYNX_LINKER` | `builtin` or `system`. Selects the linker when no `--linker` option is given. |

The order of precedence for the linker is: the `--linker` option, then
`SYNX_LINKER`, then `SYNX_RUNTIME_LIB`, then the built-in linker.
