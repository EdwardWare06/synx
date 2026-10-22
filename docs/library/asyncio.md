# std.asyncio

```synx
import std.asyncio;
import std.asyncio.{AsyncFile, Error};
```

This module runs file and standard input operations from async code. Each
operation calls the matching blocking function of [std.fs](fs.md) or
[std.io](io.md) as a job on a [BlockingPool](task.md#blockingpool). The async
worker that awaits the result does not block.

Each function takes a [BlockingSpawner](task.md#blockingspawner) and owned
arguments, and returns a `SendTask`. The job is queued when the task is first
polled. Await the task inside an `async send` function, or run it with
[block_on_send](task.md#block_on_send). Dropping the task while it waits
cancels the job if the job has not started.

The pool has a fixed number of workers. Each running operation uses one
worker until the system call returns.

## Types

### Error

```synx
enum Error { Io(fs.IoError), Closed, Cancelled }
```

The error of every operation in this module.

- `Io` holds the [IoError](fs.md#ioerror) from the file or input call.
- `Closed` means the pool was shut down before the job was queued.
- `Cancelled` means the job was cancelled before it ran, for example by a
  pool shutdown.

### AsyncFile

```synx
struct AsyncFile
```

An open file that async tasks share. The file is held in a
[Mutex](sync.md#mutex), and each operation runs as one job on the pool. Two
operations on the same file never run at the same time. The order of
operations that different tasks start at the same time is not defined.

Clones share the same file. After [close](#asyncfileclose), each operation
on any clone fails with `Error.Io` of kind `ErrorKind.Closed`. When the last
clone is dropped, the file is closed.

`AsyncFile` is Send and Owned, so an `async send` function can take it as a
parameter.

### AsyncFile.open

```synx
SendTask<Result<AsyncFile, Error>> open(BlockingSpawner pool, String path)
```

Opens the file at *path* for reading with `fs.File.open`. Later operations on
the file use *pool*.

### AsyncFile.create

```synx
SendTask<Result<AsyncFile, Error>> create(BlockingSpawner pool, String path)
```

Opens the file at *path* for writing with `fs.File.create`.

### AsyncFile.open_read_write

```synx
SendTask<Result<AsyncFile, Error>> open_read_write(BlockingSpawner pool, String path)
```

Opens an existing file at *path* for reading and writing with
`fs.File.open_read_write`.

### AsyncFile.append

```synx
SendTask<Result<AsyncFile, Error>> append(BlockingSpawner pool, String path)
```

Opens the file at *path* for appending with `fs.File.append`.

### AsyncFile.clone

```synx
AsyncFile clone(&const self)
```

Returns a new handle to the same open file and pool.

### AsyncFile.position

```synx
SendTask<Result<ulong, Error>> position(&const self)
```

Returns the current byte offset in the file.

### AsyncFile.read

```synx
SendTask<Result<Vec<byte>, Error>> read(&const self, usize length)
```

Reads up to *length* bytes with `fs.File.read`. One call reads at most 65536
bytes. An empty vector means the end of the file.

### AsyncFile.read_to_end

```synx
SendTask<Result<Vec<byte>, Error>> read_to_end(&const self)
```

Reads all bytes from the current position to the end of the file.

### AsyncFile.read_text

```synx
SendTask<Result<String, Error>> read_text(&const self)
```

Reads from the current position to the end of the file and decodes the bytes
as UTF-8. Fails with `Error.Io` of kind `ErrorKind.InvalidUtf8` if the bytes
are not valid UTF-8.

### AsyncFile.write

```synx
SendTask<Result<bool, Error>> write(&const self, Vec<byte> bytes)
```

Writes all of *bytes* with `fs.File.write_all`. Returns `Ok(true)`.

### AsyncFile.write_text

```synx
SendTask<Result<bool, Error>> write_text(&const self, String text)
```

Writes the UTF-8 bytes of *text*. Returns `Ok(true)`.

### AsyncFile.flush

```synx
SendTask<Result<bool, Error>> flush(&const self)
```

Writes buffered data to the operating system with `fs.File.flush`.

### AsyncFile.seek

```synx
SendTask<Result<bool, Error>> seek(&const self, long offset, fs.SeekFrom origin)
```

Moves the current position to *offset* bytes from *origin*.

### AsyncFile.close

```synx
SendTask<Result<bool, Error>> close(&const self)
```

Closes the file for all clones. Returns the result of `fs.File.close`. A
second close fails with `Error.Io` of kind `ErrorKind.Closed`.

## Functions

### read

```synx
SendTask<Result<Vec<byte>, Error>> read(BlockingSpawner pool, String path)
```

Reads the whole file at *path* with `fs.read`.

### read_text

```synx
SendTask<Result<String, Error>> read_text(BlockingSpawner pool, String path)
```

Reads the whole file at *path* as UTF-8 text with `fs.read_text`.

### write

```synx
SendTask<Result<bool, Error>> write(BlockingSpawner pool, String path, Vec<byte> bytes)
```

Creates or replaces the file at *path* and writes *bytes* with `fs.write`.

### write_text

```synx
SendTask<Result<bool, Error>> write_text(BlockingSpawner pool, String path, String text)
```

Creates or replaces the file at *path* and writes *text* with
`fs.write_text`.

### read_line

```synx
SendTask<Result<String?, Error>> read_line(BlockingSpawner pool)
```

Reads one line from standard input with [io.read_line](io.md#read_line).
Returns `Ok(Some(line))` for a line and `Ok(None)` at the end of the input.
Two jobs that read at the same time each receive complete lines.

## Example

Write, append, and read a file from one task:

```synx
import std.task.pool.{BlockingPool, BlockingSpawner};
import std.task.block_on_send;
import std.asyncio;
import std.fs;

async send Result<String, asyncio.Error> roundtrip(BlockingSpawner io) {
    (await asyncio.write_text(io.clone(), String.from("notes.txt"), String.from("alpha\n")))?;
    var file = (await asyncio.AsyncFile.append(io.clone(), String.from("notes.txt")))?;
    (await file.write_text(String.from("beta\n")))?;
    (await file.close())?;
    return await asyncio.read_text(io, String.from("notes.txt"));
}

int main() {
    match BlockingPool.new(2) {
        Ok(io) => {
            match block_on_send(roundtrip(io.spawner())) {
                Ok(text) => print(text.trim()),
                Err(_) => print("I/O failed"),
            }
            match block_on_send(asyncio.read(io.spawner(), String.from("missing.txt"))) {
                Err(asyncio.Error.Io(error)) => match error.kind {
                    fs.ErrorKind.NotFound => print("missing.txt: not found"),
                    _ => print("missing.txt: other I/O error"),
                },
                Err(_) => print("pool error"),
                Ok(_) => print("unexpected"),
            }
            fs.remove("notes.txt");
            io.shutdown();
            match block_on_send(asyncio.read_text(io.spawner(), String.from("notes.txt"))) {
                Err(asyncio.Error.Closed) => print("pool closed"),
                Err(_) => print("other error"),
                Ok(_) => print("unexpected"),
            }
        },
        Err(_) => return 1,
    }
    return 0;
}
```

`?` binds tighter than `await`, so the `await` expression is in parentheses.

Output:

```text
alpha
beta
missing.txt: not found
pool closed
```

Eight pool tasks write to one shared file:

```synx
import std.task.pool.{Executor, BlockingPool, Join};
import std.task.block_on_send;
import std.asyncio.AsyncFile;
import std.fs;

async send bool add_line(AsyncFile file, int id) {
    match await file.write_text("line {id}\n") {
        Ok(_) => return true,
        Err(_) => return false,
    }
}

int run(&const BlockingPool io, &const Executor executor) {
    var file = match block_on_send(AsyncFile.create(io.spawner(), String.from("log.txt"))) {
        Ok(value) => value,
        Err(_) => return 1,
    };
    var joins = Vec<Join<bool>>.new();
    for (id in 0..8) {
        match executor.spawn(add_line(file.clone(), id)) {
            Ok(join) => joins.push(join),
            Err(_) => return 1,
        }
    }
    int written = 0;
    while (!joins.is_empty()) {
        match joins.pop().unwrap().join() {
            Ok(true) => { written += 1; },
            _ => {},
        }
    }
    block_on_send(file.close());
    match fs.read_text("log.txt") {
        Ok(text) => print("{written} writes, {text.len()} bytes"),
        Err(_) => return 1,
    }
    fs.remove("log.txt");
    return 0;
}

int main() {
    match BlockingPool.new(2) {
        Ok(io) => match Executor.new(4) {
            Ok(executor) => return run(&const io, &const executor),
            Err(_) => return 1,
        },
        Err(_) => return 1,
    }
}
```

Output:

```text
8 writes, 56 bytes
```
