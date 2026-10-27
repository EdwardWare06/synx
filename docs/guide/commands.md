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
| [lsp](#lsp) | Start the language server. |
| [fmt](#fmt) | Format source files. |
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

The default build includes the Synx runtime in the executable. Copy that
executable to another computer with the same supported OS and architecture.
The target computer needs no Synx installation. Normal OS libraries are
still required. Linux programs require glibc 2.34 or later.

Options can come before or after the file name:

| Option | Effect |
|--------|--------|
| `-o <path>` | Write the executable to *path*. The folder must exist. |
| `--release` | Remove integer overflow checks. Addition, subtraction, multiplication, and negation wrap. Shift counts are masked to the operand width. Division by zero still panics. Bounds checks stay. |
| `--keep-obj` | Keep the object file (`<output>.obj` on Windows, `<output>.o` elsewhere). |
| `--no-regalloc` | Keep all local values on the stack. This option is for compiler testing. |
| `--standalone` | Include the runtime in the executable. This is the default with the built-in linker. |
| `--shared-runtime` | Use a separate Synx runtime library and copy it beside the executable. |
| `--linker=builtin` | Link with the built-in linker. This is the default. |
| `--linker=system` | Link with the system linker: `link` on Windows, `cc` elsewhere. The linker must be on `PATH`. |
| `-v`, `--verbose` | Print the number of functions, the code size, and the link step to standard error. |

Example:

```text
> synx build hello.snx -o out/hello.exe -v
Dendrite IR: 1 functions
Axon code generator: x86_64-windows, 207 bytes of code
link: built-in linker, standalone runtime
Built out/hello.exe
```

## run

```text
synx run <file.snx> [options] [-- arguments...]
```

Builds and runs a native program. Generated files stay in a private per-user
cache. The source folder receives no executable, object, or runtime library.
An unchanged program reuses its cached executable. The compiler checks the
contents of the sources, imports, compiler, and runtime before reuse.

The program inherits the working directory and standard input, output, and
error. Arguments after `--` pass to the program as separate arguments:

```text
synx run main.snx -- "two words" --example
```

Build options also apply to `run`. `--no-cache` forces a fresh private build
and removes its executable after the program exits, unless `--keep-obj` is
set. `-o <path>` writes the requested executable and bypasses cache reuse.
`--verbose` reports cache hits and misses. The system linker always builds
again because the cache does not identify the host toolchain.

The default cache folder is `%LOCALAPPDATA%/Synx/cache/run` on Windows,
`~/Library/Caches/Synx/run` on macOS, and `$XDG_CACHE_HOME/synx/run` on Linux
(or `~/.cache/synx/run` when `XDG_CACHE_HOME` is absent or relative).
`SYNX_CACHE_DIR` selects another folder. Cache files can be removed when
programs from that cache are no longer running.

`synx run` exits with the exit status of the program. A build failure exits
with status 1.

## check

```text
synx check <file.snx>
synx check <file.snx> --json
```

Runs the checks of a build: names, types, ownership, and borrows. It does
not generate code. On success, it prints `<file.snx>: ok` and exits with
status 0. On errors, it prints the diagnostics and exits with status 1.

With `--json`, it writes one JSON object to standard output instead, and
the exit status is the same. Editors use this form.

```json
{"version":1,"ok":false,"diagnostics":[
  {"file":"main.snx","severity":"error","code":"SNX0402",
   "message":"type mismatch in variable initializer: expected 'int', found 'bool'",
   "range":{"start":{"line":1,"column":22},"end":{"line":1,"column":26}}}]}
```

| Field | Meaning |
|-------|---------|
| `version` | Format version, 1. |
| `ok` | `true` when there are no errors. |
| `error` | Present only when the check could not start, for example `"cannot read file"`. |
| `file` | The file as the compiler opened it: the given path, or the path of an imported module. |
| `severity` | `error`, `warning`, or `note`. |
| `code` | The diagnostic code, such as `SNX0402`, or an empty string. |
| `range` | One-based lines and one-based columns in UTF-8 bytes. The end is exclusive. |

## fmt

```text
synx fmt [--check | --stdout] <file.snx>...
synx fmt --stdin [--check]
```

Formats named files in place. `--check` returns status 1 when changes are
needed and writes no files. `--stdout` writes one formatted file to stdout.
`--stdin` reads source through EOF. Syntax and I/O errors return status 2.
The style uses four spaces, same-line braces, and a soft 100-column limit.
See [Formatting source](formatting.md) for the rules and editor setup.

## version

```text
synx version
synx --version
```

Prints the version and the host operating system, for example
`Synx 0.2.0 (windows)`. The host is `windows`, `linux`, or `macos`.

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
| `SYNX_RUNTIME_HOST` | Path to the native runtime template used for standalone builds. |
| `SYNX_CACHE_DIR` | Private output folder for `synx run`. |
| `SYNX_RUNTIME_SHARED` | Path to the runtime library that `--shared-runtime` uses and copies next to the program. |
| `SYNX_RUNTIME_LIB` | Path to the static runtime library (`synx_rt.lib` or `libsynx_rt.a`). When it is set, the default linker is the system linker. |
| `SYNX_LINKER` | `builtin` or `system`. Selects the linker when no `--linker` option is given. |

The order of precedence for the linker is: the `--linker` option, then
`SYNX_LINKER`, then `SYNX_RUNTIME_LIB`, then the built-in linker.

## lsp

```text
synx lsp
```

Starts the language server over standard input and standard output. Messages
use JSON-RPC 2.0 with LSP Content-Length headers. Standard output contains
protocol messages only. The server uses zero-based UTF-16 positions.

The server provides live diagnostics, type and signature hover, definitions,
references, completion, function argument hints, and symbol rename. Open
buffers override files on disk, including imported modules. Source files are
never saved by the server. Changes to imported files invalidate analyses.

The VS Code extension starts the server through the configured compiler.
Other LSP clients can launch `synx lsp` with a stdio transport. Clients can
supply `initializationOptions.files`, an array of workspace file URIs, to
include unopened importers in references and rename. The server uses the nearest
ancestor `main.snx` as the program context when it imports the active file.
`initializationOptions.entryPoints` can supply other entry file URIs.

Rename requires a clean affected compilation. It rejects keywords, name
conflicts, and uses that the semantic index cannot resolve. A shorthand that
binds two symbols to one token must be expanded before rename. Edits carry
open document versions; the client applies them. The conflict check is
conservative and can reject a name used in an unrelated scope.

`synx lsp --probe` prints `synx-lsp-1` and exits. Editors use this to detect
server support. Compilers without this command retain checks on saved files.
