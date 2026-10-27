# The Synx standard library

The standard library is written in Synx. Its source is in the `std` folder of
the installation. A small C runtime supplies memory allocation, operating
system calls, and number formatting.

The [prelude](prelude.md) is available in every file without an import. Other
modules are imported by name:

```synx
import std.fs;
import std.collections.{HashMap, sort};
```

## Core

| Module | Contents |
|--------|----------|
| [Prelude](prelude.md) | `Option`, `Result`, `String`, `Vec`, `Box`, the core traits, `print`, `panic` |
| [std.iter](iter.md) | Iterator adapters and borrowed slice iteration |
| [std.mem](mem.md) | `replace`, `swap`, `drop` |
| [std.hash](hash.md) | The `Hash` trait and the default hasher |
| [std.collections](collections.md) | `HashMap` and `sort` |

## Text and numbers

| Module | Contents |
|--------|----------|
| [std.string](string.md) | The `String` type |
| [std.text](text.md) | Searching, replacing, splitting, case changes, number parsing |
| [std.utf8](utf8.md) | UTF-8 validation and decoding |
| [std.math](math.md) | Numeric functions and `Vec3` |
| [std.binary](binary.md) | Integer encoding in little-endian and big-endian byte order |

## Input, output, and the system

| Module | Contents |
|--------|----------|
| [std.io](io.md) | Standard input and output |
| [std.fs](fs.md) | Files, directories, metadata, `IoError` |
| [std.path](path.md) | `Path` and path operations for Windows and Unix |
| [std.env](env.md) | Command-line arguments, environment variables, the current program |
| [std.process](process.md) | Starting other programs |
| [std.time](time.md) | Clocks and durations |

## Concurrency

| Module | Contents |
|--------|----------|
| [std.thread](thread.md) | Threads and `JoinHandle` |
| [std.sync](sync.md) | Atomics, `Arc`, `Mutex`, `Condvar`, `Cell` |
| [std.channel](channel.md) | Channels between threads |
| [std.task](task.md) | Async executors, `block_on`, the worker pool |
| [std.asyncio](asyncio.md) | Async file and input operations |
