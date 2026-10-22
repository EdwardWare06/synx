# std.process

```synx
import std.process;
import std.process.{Command, ExitStatus, exit};
```

This module ends the current process and runs other executables. A child
process gets its arguments as separate values. No shell runs, so there is no
variable substitution, wildcard expansion, or redirection. Errors use
[IoError](fs.md#ioerror) from `std.fs`.

## Types

### ExitStatus

```synx
struct ExitStatus {
    ulong? code;
    int? signal;
}
```

The result of a child process that ran to its end. Exactly one field has a
value.

*code* holds the exit code of a normal exit. On Linux and macOS, the value
is from 0 to 255. On Windows, the value is the full unsigned 32-bit exit
code. *signal* holds the number of the POSIX signal that stopped the child.
It is always `None` on Windows.

### ExitStatus.success

```synx
bool success(&const self)
```

Returns `true` if *code* is `Some(0)`. Returns `false` for any other code
and for a signal.

### Command

```synx
struct Command
```

A program name and a list of arguments. The fields are private. The child
inherits the environment, the working directory, and the standard input,
output, and error streams of the parent. Files that the program opened with
[std.fs](fs.md) do not pass to the child.

### Command.new

```synx
Command new(&const str program)
```

Returns a `Command` that holds a copy of *program* and no arguments.

If *program* contains no path separator, the operating system searches for
it. On Linux and macOS, `posix_spawnp` searches the directories in `PATH`.
On Windows, `CreateProcessW` searches, and its search can include the
current directory. Use an absolute path to select one specific executable.

### Command.arg

```synx
void arg(&self, &const str argument)
```

Adds a copy of *argument* to the end of the argument list.

### Command.status

```synx
Result<ExitStatus, IoError> status(&const self)
```

Starts the program with the arguments, waits for it to end, and returns its
[ExitStatus](#exitstatus). A child that ends with a nonzero code gives
`Ok`. The same `Command` can run more than once.

The call blocks the thread until the child ends. Call it from ordinary code,
from an OS thread, or from a blocking pool job. On an async executor worker,
it panics with `blocking join on async worker; await the handle` before the
child starts.

On Windows, the library puts each argument in quotes and escapes quotes and
backslashes with the rules of the Microsoft C runtime. A program that reads
its command line with other rules, such as a command interpreter, can see
different arguments. The full command line must be shorter than 32767
UTF-16 units. On Linux and macOS, the argument list goes to `posix_spawnp`
with no change.

Errors:

- `ErrorKind.NotFound` if the program does not exist.
- `ErrorKind.InvalidInput` if *program* is empty, or if *program* or an
  argument contains a zero byte.
- `ErrorKind.InvalidInput` on Windows if *program* ends with `.bat` or
  `.cmd`. Trailing spaces and dots are ignored for this test. To run a batch
  file, run the command interpreter and give it the file as an argument.
- `ErrorKind.Other` on Windows if the command line is too long.
- A native error if the child cannot start or the wait fails.

## Functions

### exit

```synx
void exit(int code)
```

Ends the process at once with the exit status *code*. The function does not
return. It calls the C `exit` function. Synx values that are still live are
not dropped.

## Example

The program runs a second copy of itself as a child. The child prints its
argument and exits with code 3:

```synx
import std.env;
import std.process;
import std.process.Command;

int main() {
    var args = env.args();
    if (args.len() > 1) {
        print("child: {args[1]}");
        process.exit(3);
    }

    var path = match env.exe_path() {
        Ok(path) => path,
        Err(_) => { print("no executable path"); return 1; },
    };
    var command = Command.new(path.as_str());
    command.arg("one argument with spaces and \"quotes\"");
    match command.status() {
        Ok(status) => {
            match status.code {
                Some(code) => print("exit code {code}"),
                None => print("no exit code"),
            }
            print("success: {status.success()}");
        }
        Err(_) => print("cannot run the child"),
    }

    var missing = Command.new("synx-doc-missing-program");
    match missing.status() {
        Ok(_) => print("unexpected"),
        Err(error) => match error.kind {
            NotFound => print("missing program: NotFound"),
            _ => print("missing program: other error"),
        },
    }
    return 0;
}
```

Output:

```text
child: one argument with spaces and "quotes"
exit code 3
success: false
missing program: NotFound
```
