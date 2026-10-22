# 13. Modules and files

## One file is one module

Each `.snx` file is a module. The name of the module is the name of the file
without `.snx`. Put these two files in the same folder.

`units.snx`:

```synx
pub const KG_PER_LB = 0.45359237;

pub double to_kg(double pounds) {
    return round_tenth(pounds * KG_PER_LB);
}

double round_tenth(double value) {
    return (value * 10.0 + 0.5) as int as double / 10.0;
}
```

`main.snx`:

```synx
import units;

main() {
    print("225 lb = {units.to_kg(225.0)} kg");
    print("1 lb = {units.KG_PER_LB} kg");
}
```

Build the file that holds `main`. The compiler finds and loads the other
modules:

```text
synx run main.snx
```

```text
225 lb = 102.1 kg
1 lb = 0.45359237 kg
```

`import units;` loads `units.snx` and binds the name `units`. Items of the
module are then written `units.to_kg` and `units.KG_PER_LB`.

## Importing items

An import can name one item. The item is then used without the module name:

```synx
import units.to_kg;

main() {
    print("{to_kg(135.0)} kg");
}
```

```text
61.2 kg
```

Braces import several items from one module:

```synx
import units.{to_kg, KG_PER_LB};

main() {
    print("{to_kg(315.0)} kg at {KG_PER_LB} kg per lb");
}
```

```text
142.9 kg at 0.45359237 kg per lb
```

An import binds names in its own file only. If `main.snx` imports `units`,
another module that uses `units` must import it too.

## Visibility

Items are private to their module by default. `pub` makes an item visible to
other modules. In `units.snx`, `to_kg` is public and `round_tenth` is private.

Fields and methods of a struct have their own `pub` markers.

`counter.snx`:

```synx
pub struct Counter {
    pub String label;
    int count;

    pub Counter new(&const str label) {
        return Counter { label: String.from(label), count: 0 };
    }
    pub void add(&self, int amount) {
        self.count += amount;
    }
    pub int total(&const self) {
        return self.count;
    }
}
```

`main.snx`:

```synx
import counter.Counter;

main() {
    var reps = Counter.new("reps");
    reps.add(5);
    reps.add(3);
    print("{reps.label}: {reps.total()}");
}
```

```text
reps: 8
```

The field `count` is private. Another module cannot read it, assign it, or
use it in a struct literal. It must use the public methods:

```synx
import counter.Counter;

main() {
    var reps = Counter.new("reps");
    reps.count = 100;
}
```

```text
main.snx:5:10: error[SNX0304]: 'count' is private to module 'counter'
5 |     reps.count = 100;
  |          ^^^^^
1 error
```

## Modules in folders

A module in a subfolder has a dotted name. The file `game/loot.snx` is the
module `game.loot`. Every import path starts at the folder of the file that
holds `main`. This rule also applies to imports inside a subfolder.

The folder layout:

```text
main.snx
game/items.snx
game/loot.snx
```

`game/items.snx`:

```synx
pub struct Item {
    pub String name;
    pub int value;
}
```

`game/loot.snx`:

```synx
import game.items.Item;

pub Item roll(int seed) {
    if (seed % 2 == 0) {
        return Item { name: String.from("Mantis Blades"), value: 1200 };
    }
    return Item { name: String.from("Sandevistan"), value: 3400 };
}
```

`main.snx`:

```synx
import game.loot;

main() {
    var item = loot.roll(3);
    print("{item.name}: {item.value} eddies");
}
```

```text
Sandevistan: 3400 eddies
```

`game/loot.snx` writes `import game.items.Item;`. The shorter
`import items.Item;` is an error, because the path does not start at the
folder of `main.snx`.

## A folder as a module

The file `mod.snx` in a folder is the module with the name of the folder.
`game/mod.snx` is the module `game`. `pub import` makes an imported item
part of the public items of the module. Other modules can then import the
item from `game` directly.

`game/mod.snx`:

```synx
pub import game.items.Item;
pub import game.loot.roll;

pub const VERSION = 2;
```

`main.snx`:

```synx
import game.{Item, roll, VERSION};

main() {
    Item item = roll(4);
    print("{item.name}: {item.value} eddies");
    print("data version {VERSION}");
}
```

```text
Mantis Blades: 1200 eddies
data version 2
```

## The standard library

Modules whose path starts with `std` come from the standard library. The
compiler finds the `std` folder of its installation. The environment variable
`SYNX_STD_PATH` selects a different folder. A file in your project cannot
replace a `std` module.

The prelude is part of the standard library. Every module can use its names,
such as `print`, `String`, `Vec`, `Option`, and `Result`, without an import.
Other library modules need an import:

```synx
import std.math;
import std.text.to_ascii_uppercase;

main() {
    print("{math.sqrt(2.25)}");
    print(to_ascii_uppercase("warmup"));
}
```

```text
1.5
WARMUP
```

The rest of this chapter uses `std.fs`, `std.path`, `std.env`, and
`std.process`. The [library reference](../../library/index.md) describes
every module.

## Reading and writing files

`std.fs.write_text` makes a file and writes text to it. `std.fs.read_text`
reads a whole file as a `String`. Both return a `Result`:

```synx
import std.fs;
import std.text;

main() {
    match fs.write_text("playlist.txt", "Take Me Back to Eden\nSomewhere I Belong\nLife Is Strange\n") {
        Ok(_) => print("saved"),
        Err(_) => print("cannot write playlist.txt"),
    }
    match fs.read_text("playlist.txt") {
        Ok(contents) => {
            var lines = text.lines(contents);
            for (line in &const lines) {
                if (!line.is_empty()) {
                    print("- {line}");
                }
            }
        }
        Err(_) => print("cannot read playlist.txt"),
    }
}
```

```text
saved
- Take Me Back to Eden
- Somewhere I Belong
- Life Is Strange
```

`write_text` replaces the file if it exists. Synx writes the bytes as they
are. It does not change `\n` to `\r\n` on Windows. `std.text.lines` splits
text at each line break. The text ends with a line break, so the last line is
empty.

## File handles

`File.create` opens a file for writing. `File.open` opens a file for reading.
`File.append` writes at the end of a file. These functions and the methods of
`File` return `Result<T, IoError>`, so a function that returns a `Result`
with `IoError` can use `?`:

```synx
import std.fs.{File, IoError};

Result<bool, IoError> save() {
    var file = File.create("notes.txt")?;
    file.write_text("first line\n")?;
    file.write_text("second line\n")?;
    return file.close();
}

Result<String, IoError> load() {
    var file = File.open("notes.txt")?;
    return file.read_text();
}

main() {
    match save() {
        Ok(_) => print("saved"),
        Err(error) => print("save failed: OS code {error.os_code}"),
    }
    match load() {
        Ok(text) => print(text.trim()),
        Err(error) => print("load failed: OS code {error.os_code}"),
    }
}
```

```text
saved
first line
second line
```

A `File` owns an open file. When the `File` goes out of scope, it closes the
file. The method `close` also closes the file, and it reports an error that
the automatic close ignores. `close` takes `self`, so the file cannot be used
after the call. A successful write or close returns `Ok(true)`.

`load` does not call `close`. Its file closes when `load` returns.

## I/O errors

An `IoError` has two fields. `kind` is an `ErrorKind` value that is the same
on every operating system. `os_code` is the error number from the operating
system, or 0 for an error that the library found:

```synx
import std.fs;
import std.fs.IoError;

String describe(&const IoError error) {
    match error.kind {
        NotFound => return String.from("file not found"),
        PermissionDenied => return String.from("permission denied"),
        _ => return "I/O error {error.os_code}",
    }
}

main() {
    match fs.read_text("missing.txt") {
        Ok(text) => print(text),
        Err(error) => print("missing.txt: {describe(error)}"),
    }
}
```

```text
missing.txt: file not found
```

The other kinds are `InvalidInput`, `AlreadyExists`, `Closed`, `InvalidUtf8`,
`UnexpectedEof`, and `Other`. `read_text` gives `InvalidUtf8` when the file
is not valid UTF-8 text.

## Paths

`std.path.Path` holds the text of a path. Its methods work on the text only.
They do not look at the file system:

```synx
import std.path.Path;

main() {
    var dir = Path.from("reports");
    var file = dir.join("week1.txt");
    print(file.as_str());

    match file.file_name() {
        Some(name) => print("name: {name}"),
        None => print("no name"),
    }
    match file.extension() {
        Some(ext) => print("extension: {ext}"),
        None => print("no extension"),
    }
    match file.parent() {
        Some(parent) => print("parent: {parent.as_str()}"),
        None => print("no parent"),
    }
}
```

```text
reports/week1.txt
name: week1.txt
extension: txt
parent: reports
```

`join` adds a `/` between the parts on every system. Windows accepts `/` in
paths. `Path.from` uses the rules of the current system. `Path.with_style`
selects the Unix or Windows rules. `normalize` removes `.` parts and cancels
a name before `..`:

```synx
import std.path.{Path, Style};

main() {
    var path = Path.from("reports/./drafts/../week1.txt");
    print(path.normalize().as_str());

    var windows = Path.with_style("C:\\data\\reports", Style.Windows);
    print("{windows.is_absolute()}");
    print(windows.join("week1.txt").as_str());
    print(windows.normalize().as_str());
}
```

```text
reports/week1.txt
true
C:\data\reports/week1.txt
C:/data/reports
```

The `std.fs` functions take `&const str`. Pass `path.as_str()`.

## Folders and metadata

`fs.exists`, `fs.create_dir`, and `fs.metadata` work with folders and file
information:

```synx
import std.fs;
import std.path.Path;

main() {
    var folder = Path.from("output");
    match fs.exists(folder.as_str()) {
        Ok(true) => {},
        Ok(false) => {
            fs.create_dir(folder.as_str());
        }
        Err(_) => print("cannot check the folder"),
    }
    var file = folder.join("result.txt");
    match fs.write_text(file.as_str(), "42\n") {
        Ok(_) => print("wrote {file.as_str()}"),
        Err(_) => print("write failed"),
    }
    match fs.metadata(file.as_str()) {
        Ok(info) => print("{info.len} bytes, file: {info.is_file}, folder: {info.is_dir}"),
        Err(_) => print("no metadata"),
    }
}
```

```text
wrote output/result.txt
3 bytes, file: true, folder: false
```

`exists` returns `Ok(false)` for a missing path. It returns `Err` for other
problems, for example a denied permission. `create_dir` makes one folder. It
does not make the parent folders. `fs.remove`, `fs.remove_dir`, and
`fs.rename` remove and rename files and folders.

## Program arguments

`std.env.args` returns the command-line arguments as a `Vec<String>`. Index 0
holds the name of the program:

```synx
import std.env;

main() {
    var args = env.args();
    if (args.len() < 2) {
        print("usage: lifts <name> ...");
        return;
    }
    for (i in 1..args.len()) {
        print("{i}: {args[i]}");
    }
}
```

With no arguments, the program prints its usage line:

```text
usage: lifts <name> ...
```

`synx run` gives the program no arguments. Build the program, then run the
executable with three arguments:

```text
> synx build lifts.snx
Built lifts.exe
> .\lifts.exe squat "bench press" deadlift
1: squat
2: bench press
3: deadlift
```

The quotes make `bench press` one argument.

## Environment variables

`env.get` reads one environment variable. It returns
`Result<String?, IoError>`. `Ok(None)` means that the variable is not set:

```synx
import std.env;

void show(&const str name) {
    match env.get(name) {
        Ok(Some(value)) => print("{name} = {value}"),
        Ok(None) => print("{name} is not set"),
        Err(_) => print("{name} cannot be read"),
    }
}

main() {
    show("SYNX_TUTORIAL_MODE");
}
```

```text
SYNX_TUTORIAL_MODE is not set
```

When the variable has the value `strict`, the program prints
`SYNX_TUTORIAL_MODE = strict`. Synx has no function that changes the
environment.

## Running other programs

`std.process.Command` starts another program and waits for it. Each `arg`
call adds one argument. Synx passes the arguments directly. No shell reads
them, so quotes, `*`, and `$` have no special meaning.

This program runs a second copy of itself. `env.exe_path` gives the path of
the running program:

```synx
import std.env;
import std.process.Command;

int main() {
    var args = env.args();
    if (args.len() > 1) {
        print("child received: {args[1]}");
        return 3;
    }
    String program;
    match env.exe_path() {
        Ok(path) => { program = path; }
        Err(_) => {
            print("cannot find this program");
            return 1;
        }
    }
    var command = Command.new(program);
    command.arg("one argument with spaces");
    match command.status() {
        Ok(status) => {
            print("success: {status.success()}");
            match status.code {
                Some(code) => print("exit code {code}"),
                None => print("stopped by a signal"),
            }
        }
        Err(_) => print("cannot start the program"),
    }
    return 0;
}
```

```text
child received: one argument with spaces
success: false
exit code 3
```

The child uses the same console as the parent, so its output appears in the
same place. `status` returns `Err` only when the program cannot start. A
program that starts and then fails gives `Ok` with a nonzero code. On Linux
and macOS, a program that a signal stops has no code. Its `status.signal`
holds the signal number.

`status` blocks the thread until the child ends. Chapter 14 shows how to run
blocking work from async code.

## Next

[Chapter 14](14-threads-and-async.md) covers threads, shared data, channels,
and async functions.
