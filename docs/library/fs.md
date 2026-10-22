# std.fs

```synx
import std.fs;
import std.fs.{File, SeekFrom, Metadata, IoError, ErrorKind};
```

This module holds the file type `File`, the error type `IoError`, and
functions that read, write, rename, and remove files and directories. Paths
are borrowed UTF-8 text. All files are opened in binary mode. The module does
no newline conversion. For lexical path operations, see
[std.path](path.md).

On Windows, the runtime converts each path to UTF-16 and calls the wide
character functions. On Linux and macOS, the runtime passes the path bytes
to the C library. A path that contains a zero byte gives
`ErrorKind.InvalidInput`.

## Types

### ErrorKind

```synx
enum ErrorKind { Other, NotFound, PermissionDenied, InvalidInput, AlreadyExists, Closed, InvalidUtf8, UnexpectedEof }
```

The portable category of an `IoError`. The library sets the kind from the
operating system error code as follows:

| Kind | Operating system codes |
| --- | --- |
| `NotFound` | `ENOENT`, `ENOTDIR` |
| `PermissionDenied` | `EACCES`, `EPERM` |
| `InvalidInput` | `EINVAL` |
| `AlreadyExists` | `EEXIST` |
| `InvalidUtf8` | `EILSEQ` |
| `Other` | all other codes |

The library also makes errors with `os_code` zero:

- `InvalidInput` for an operation in the wrong direction, such as a write to
  a file opened with `File.open`.
- `Closed` for an operation on a closed `File`.
- `InvalidUtf8` when text from a file or from the operating system is not
  valid UTF-8.
- `UnexpectedEof` when `File.read_exact` reaches the end of the file first.
- `Other` when a write transfers zero bytes.

### IoError

```synx
struct IoError {
    ErrorKind kind;
    int os_code;
}
```

The error type of every fallible function in this module. `std.io`,
`std.env`, and `std.process` use the same type.

*kind* is the portable category. *os_code* holds the native `errno` value.
It is zero for an error that the Synx library detects itself. On Windows,
the runtime converts native error codes to `errno` values before it makes
the `IoError`.

### IoError.from_os

```synx
IoError from_os(int code)
```

Makes an `IoError` from an `errno` value that an `extern "C"` adapter
returns. The kind follows the table in [ErrorKind](#errorkind). *os_code* is
*code*.

### SeekFrom

```synx
enum SeekFrom { Start, Current, End }
```

The origin of a [File.seek](#fileseek) offset: the start of the file, the
current position, or the end of the file.

### File

```synx
struct File
```

An open file. A `File` owns its handle. When a `File` is dropped, the handle
closes and any close error is discarded. Use [File.close](#fileclose) to
receive the close error.

`File` is `Send` and `Owned`. A `File` can move to another thread. The
methods that read, write, or seek take `&self`, so they need exclusive
access.

On Windows, child processes do not inherit the handle. On Linux and macOS,
the descriptor is opened with `O_CLOEXEC`.

### File.open

```synx
Result<File, IoError> open(&const str path)
```

Opens an existing file for reading.

Errors:

- `ErrorKind.NotFound` if the file does not exist or *path* is empty.
- `ErrorKind.PermissionDenied` if access is refused.
- `ErrorKind.InvalidInput` if *path* contains a zero byte.

### File.create

```synx
Result<File, IoError> create(&const str path)
```

Opens a file for writing. The file is created if it does not exist. An
existing file is truncated to zero length. A `File` from `create` cannot
read.

Errors are the same as for [File.open](#fileopen). `NotFound` also occurs
when the parent directory does not exist.

### File.append

```synx
Result<File, IoError> append(&const str path)
```

Opens a file for writing at the end. The file is created if it does not
exist. Every write goes to the current end of the file, also after a seek.
A `File` from `append` cannot read.

Errors are the same as for [File.create](#filecreate).

### File.open_read_write

```synx
Result<File, IoError> open_read_write(&const str path)
```

Opens an existing file for reading and writing. The file is not truncated.
The position starts at zero.

The `File` records the direction of the last operation. When a read follows
a write, the read flushes the stream first. When a write follows a read, the
write seeks zero bytes from the current position first. A successful
[seek](#fileseek) clears the recorded direction.

Errors are the same as for [File.open](#fileopen).

### File.read

```synx
Result<Vec<byte>, IoError> read(&self, usize limit)
```

Reads up to *limit* bytes from the current position and returns them. One
call reads at most 65536 bytes. The result can be shorter than *limit*. An
empty vector means the end of the file, when *limit* is not zero. A *limit*
of zero returns an empty vector and reads nothing.

Errors:

- `ErrorKind.Closed` if the file is closed.
- `ErrorKind.InvalidInput` if the file was opened with `create` or `append`.
- A native error if the read fails. The bytes that were read are discarded.

### File.read_exact

```synx
Result<Vec<byte>, IoError> read_exact(&self, usize length)
```

Reads exactly *length* bytes. The method calls [read](#fileread) until it
has *length* bytes.

Errors:

- `ErrorKind.UnexpectedEof` if the file ends first. The bytes that were read
  are discarded, and the position stays after them.
- All errors of [File.read](#fileread).

### File.read_to_end

```synx
Result<Vec<byte>, IoError> read_to_end(&self)
```

Reads from the current position to the end of the file and returns the
bytes. The method reads in blocks of 4096 bytes. It sets no size limit.

Errors are the same as for [File.read](#fileread).

### File.read_text

```synx
Result<String, IoError> read_text(&self)
```

Reads to the end of the file, like [read_to_end](#fileread_to_end), and
returns the bytes as a `String`. No newline conversion is done.

Errors:

- `ErrorKind.InvalidUtf8` if the bytes are not valid UTF-8.
- All errors of [File.read](#fileread).

### File.write_all

```synx
Result<bool, IoError> write_all(&self, &const Vec<byte> bytes)
```

Writes all of *bytes* at the current position. Returns `Ok(true)`.

Errors:

- `ErrorKind.Closed` if the file is closed.
- `ErrorKind.InvalidInput` if the file was opened with `open`.
- `ErrorKind.Other` with *os_code* zero if the stream accepts zero bytes.
- A native error if the write fails. Part of the data can be written
  before the error.

The stream is buffered. A write error can also appear later, from
[flush](#fileflush) or [close](#fileclose).

### File.write_text

```synx
Result<bool, IoError> write_text(&self, &const str text)
```

Writes the UTF-8 bytes of *text*, like [write_all](#filewrite_all). No line
feed is added.

### File.flush

```synx
Result<bool, IoError> flush(&self)
```

Writes buffered data to the operating system. Returns `Ok(true)`.

Errors:

- `ErrorKind.Closed` if the file is closed.
- A native error if the buffered write fails.

### File.close

```synx
Result<bool, IoError> close(self)
```

Closes the file and returns `Ok(true)`. The method takes ownership of the
`File`, so the file cannot be used after the call. Buffered data is written
before the handle closes.

Errors:

- A native error if the buffered write or the close fails. The handle is
  released also when an error occurs.

### File.seek

```synx
Result<bool, IoError> seek(&self, long offset, SeekFrom from)
```

Moves the position to *offset* bytes from the origin *from*. *offset* can
be negative. Returns `Ok(true)`.

Errors:

- `ErrorKind.Closed` if the file is closed.
- `ErrorKind.InvalidInput` if the new position is before the start of the
  file.

### File.position

```synx
Result<ulong, IoError> position(&self)
```

Returns the current position as a byte offset from the start of the file.

Errors:

- `ErrorKind.Closed` if the file is closed.
- A native error if the position cannot be read.

### Metadata

```synx
struct Metadata {
    bool is_file;
    bool is_dir;
    ulong len;
}
```

The result of [metadata](#metadata-1). *is_file* is `true` for a regular
file. *is_dir* is `true` for a directory. Both are `false` for other kinds,
such as a device or a pipe. *len* is the size of a file in bytes. For other
kinds, the value of *len* depends on the operating system.

## Functions

### read

```synx
Result<Vec<byte>, IoError> read(&const str path)
```

Opens the file at *path*, reads all of its bytes, and closes it.

Errors are the same as for [File.open](#fileopen) and
[File.read](#fileread).

### read_text

```synx
Result<String, IoError> read_text(&const str path)
```

Opens the file at *path*, reads all of its bytes as UTF-8 text, and closes
it.

Errors are the same as for [File.open](#fileopen) and
[File.read_text](#fileread_text).

### write

```synx
Result<bool, IoError> write(&const str path, &const Vec<byte> bytes)
```

Creates or truncates the file at *path*, writes *bytes*, and closes the
file. Returns `Ok(true)`. The function reports the errors of the write and
of the close.

If the write fails, the file can remain with part of the data.

### write_text

```synx
Result<bool, IoError> write_text(&const str path, &const str text)
```

Creates or truncates the file at *path*, writes the UTF-8 bytes of *text*,
and closes the file, like [write](#write).

### remove

```synx
Result<bool, IoError> remove(&const str path)
```

Removes the file at *path*. Returns `Ok(true)`.

On Windows, a directory gives `ErrorKind.PermissionDenied`. On Linux and
macOS, the function uses the C `remove` function, which also removes an
empty directory.

Errors:

- `ErrorKind.NotFound` if *path* does not exist or is empty.
- `ErrorKind.PermissionDenied` if access is refused. On Windows, a file that
  is open in this or another process can also give this error.

### metadata

```synx
Result<Metadata, IoError> metadata(&const str path)
```

Returns the [Metadata](#metadata) of the file or directory at *path*. The
function follows symbolic links.

Errors:

- `ErrorKind.NotFound` if *path* does not exist or is empty. On Windows, an
  invalid file name also gives `NotFound`.
- `ErrorKind.PermissionDenied` if access is refused.

### exists

```synx
Result<bool, IoError> exists(&const str path)
```

Returns `Ok(true)` if [metadata](#metadata-1) succeeds for *path*. Returns
`Ok(false)` if `metadata` gives `ErrorKind.NotFound`. All other errors are
returned as errors.

### create_dir

```synx
Result<bool, IoError> create_dir(&const str path)
```

Creates one directory. The parent directory must exist. Returns `Ok(true)`.

Errors:

- `ErrorKind.AlreadyExists` if *path* exists.
- `ErrorKind.NotFound` if the parent does not exist or *path* is empty.

### remove_dir

```synx
Result<bool, IoError> remove_dir(&const str path)
```

Removes an empty directory. Returns `Ok(true)`.

Errors:

- `ErrorKind.NotFound` if *path* does not exist or is empty.
- `ErrorKind.Other` if the directory is not empty. *os_code* holds
  `ENOTEMPTY`.

### rename

```synx
Result<bool, IoError> rename(&const str from, &const str to)
```

Renames or moves the file or directory *from* to *to*. Returns `Ok(true)`.

The result for an existing *to* depends on the operating system. On Windows,
an existing *to* gives `ErrorKind.AlreadyExists`. On Linux and macOS, the
function uses the C `rename` function, which replaces an existing file.

Errors:

- `ErrorKind.NotFound` if *from* does not exist, or if either path is
  empty.
- `ErrorKind.PermissionDenied` if access is refused. On Windows, a file that
  is open can give this error.

### current_dir

```synx
Result<String, IoError> current_dir()
```

Returns the current working directory as an absolute path.

Errors:

- `ErrorKind.InvalidUtf8` on Linux and macOS if the path is not valid
  UTF-8. On Windows, a path that cannot convert to UTF-8 gives
  `ErrorKind.InvalidInput`.
- A native error if the directory cannot be read.

### absolute

```synx
Result<String, IoError> absolute(&const str path)
```

Makes *path* absolute. The function does not read the target. It does not
resolve symbolic links. The target does not have to exist.

On Windows, the function uses `GetFullPathNameW`. It resolves drive-relative
paths, such as `C:file`, and removes `.` and `..` components. On Linux and
macOS, an absolute *path* is returned with no change. A relative *path* is
added to the current directory with `/`. `.` and `..` components stay in
the result.

Errors:

- `ErrorKind.InvalidInput` if *path* is empty or contains a zero byte.
- `ErrorKind.InvalidUtf8` if the result is not valid UTF-8.

### canonicalize

```synx
Result<String, IoError> canonicalize(&const str path)
```

Returns the absolute path of an existing file or directory. The function
follows symbolic links.

On Windows, the result uses the extended path prefix `\\?\`, for example
`\\?\C:\work\notes.txt`. On Linux and macOS, the function uses `realpath`.

Errors:

- `ErrorKind.NotFound` if *path* does not exist or is empty.
- `ErrorKind.InvalidInput` if *path* contains a zero byte.
- `ErrorKind.PermissionDenied` if access is refused.

For a path that does not exist, use
[Path.normalize](path.md#pathnormalize) for a lexical result.

### same_file

```synx
Result<bool, IoError> same_file(&const str a, &const str b)
```

Returns `Ok(true)` if *a* and *b* refer to the same file. The function
compares the file identity from the operating system. Two hard links to one
file give `true`. Symbolic links are followed.

On Windows, the function compares the volume serial number and the file
index. On Linux and macOS, it compares the device and inode numbers.

Errors:

- `ErrorKind.NotFound` if either path does not exist or is empty.
- `ErrorKind.InvalidInput` if either path contains a zero byte.

The result describes the files at the time of the call. The function does
not lock either path.

## Example

Write a file, add to it, read part of it, and remove it:

```synx
import std.fs;
import std.fs.{File, SeekFrom, IoError, ErrorKind};

&const str kind_name(ErrorKind kind) {
    match kind {
        NotFound => return "NotFound",
        AlreadyExists => return "AlreadyExists",
        _ => return "other",
    }
}

Result<bool, IoError> run() {
    fs.write_text("notes.txt", "alpha\n")?;
    var file = File.append("notes.txt")?;
    file.write_text("beta\n")?;
    file.close()?;

    var text = fs.read_text("notes.txt")?;
    print("text: {text.len()} bytes");
    var info = fs.metadata("notes.txt")?;
    print("is_file {info.is_file}, is_dir {info.is_dir}, len {info.len}");

    var reader = File.open("notes.txt")?;
    reader.seek(6, SeekFrom.Start)?;
    var bytes = reader.read_exact(4)?;
    print("read {bytes.len()} bytes, position {reader.position()?}");
    reader.close()?;

    fs.rename("notes.txt", "old.txt")?;
    print("notes.txt exists: {fs.exists("notes.txt")?}");
    fs.remove("old.txt")?;
    return Ok(true);
}

main() {
    match run() {
        Ok(_) => print("done"),
        Err(error) => print("error {error.os_code}"),
    }
    match fs.read_text("missing.txt") {
        Ok(_) => print("unexpected"),
        Err(error) => print("missing.txt: {kind_name(error.kind)}"),
    }
}
```

Output:

```text
text: 11 bytes
is_file true, is_dir false, len 11
read 4 bytes, position 10
notes.txt exists: false
done
missing.txt: NotFound
```

On Windows, `rename` fails with `PermissionDenied` while `reader` is open.
The program closes `reader` before the rename.

Use a directory and a file that is open for reading and writing:

```synx
import std.fs;
import std.fs.{File, SeekFrom, IoError};

Result<bool, IoError> run() {
    fs.create_dir("work")?;
    var data = Vec<byte>.new();
    for (i in 0..8) { data.push(i as byte); }
    fs.write("work/data.bin", &data)?;

    var file = File.open_read_write("work/data.bin")?;
    file.seek(-2, SeekFrom.End)?;
    var tail = file.read(10)?;
    print("tail: {tail[0]} {tail[1]}");
    file.seek(0, SeekFrom.Start)?;
    var patch = Vec<byte>.new();
    patch.push(255);
    file.write_all(&patch)?;
    file.close()?;

    var bytes = fs.read("work/data.bin")?;
    print("first byte: {bytes[0]}, length: {bytes.len()}");

    match fs.create_dir("work") {
        Ok(_) => print("unexpected"),
        Err(error) => match error.kind {
            AlreadyExists => print("work: AlreadyExists"),
            _ => print("work: other error"),
        },
    }
    fs.remove("work/data.bin")?;
    fs.remove_dir("work")?;
    print("work exists: {fs.exists("work")?}");
    return Ok(true);
}

main() {
    match run() {
        Ok(_) => {},
        Err(error) => print("I/O error, os_code {error.os_code}"),
    }
}
```

Output:

```text
tail: 6 7
first byte: 255, length: 8
work: AlreadyExists
work exists: false
```
