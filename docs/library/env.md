# std.env

```synx
import std.env;
import std.env.{args, get, exe_path};
```

This module reads the command line arguments, the environment variables,
and the path of the running executable. The functions do not change the
process environment. Errors use [IoError](fs.md#ioerror) from `std.fs`.

On Windows, the runtime reads arguments, variables, and paths as UTF-16 and
converts them to UTF-8. On Linux and macOS, the runtime reads the native
bytes and the library checks that they are valid UTF-8.

## Functions

### args

```synx
Vec<String> args()
```

Returns the command line arguments as owned strings. Index zero holds the
name of the executable, in the form that the operating system gives. The
arguments that the user typed start at index one.

On Windows, the runtime converts the command line before `main` starts.
If the command line is not valid UTF-16, the program panics with
`command line is not valid Unicode` before `main` runs. On Linux and macOS,
`args` panics with `command line argument is not valid UTF-8` if an argument
is not valid UTF-8.

### get

```synx
Result<String?, IoError> get(&const str name)
```

Reads the environment variable *name*.

Returns `Ok(Some(value))` if the variable is set. An empty value gives
`Ok(Some(""))`. Returns `Ok(None)` if the variable is not set.

On Windows, the function uses `GetEnvironmentVariableW`. On Linux and macOS,
it uses `getenv`.

Errors:

- `ErrorKind.InvalidInput` if *name* is empty, or contains `=` or a zero
  byte.
- `ErrorKind.InvalidUtf8` if the value is not valid UTF-8.

The function reads the environment without a lock. C code that changes the
environment while other threads read it must synchronize those changes
itself.

### exe_path

```synx
Result<String, IoError> exe_path()
```

Returns the absolute path of the running executable.

On Linux, the function reads the link `/proc/self/exe`. On macOS, it uses
`_NSGetExecutablePath` and resolves symbolic links with `realpath`. On
Windows, it uses `GetModuleFileNameW` and returns the path that was used to
load the executable. Windows does not resolve symbolic links.

Errors:

- `ErrorKind.InvalidUtf8` if the path is not valid UTF-8.
- A native error if the path cannot be read.

## Example

```synx
import std.env;

main() {
    var args = env.args();
    print("{args.len() - 1} arguments");
    for (i in 1..args.len()) {
        print("argument {i}: {args[i]}");
    }
    match env.get("SYNX_DOC_COLOR") {
        Ok(Some(value)) => print("color: {value}"),
        Ok(None) => print("color: not set"),
        Err(_) => print("color: cannot read"),
    }
}
```

Run with the arguments `red` and `two words`, and with `SYNX_DOC_COLOR` not
set, the output is:

```text
2 arguments
argument 1: red
argument 2: two words
color: not set
```

Run with no arguments and with `SYNX_DOC_COLOR` set to `blue`, the output
is:

```text
0 arguments
color: blue
```
