# 14. Threads and async

Synx has two ways to run work at the same time. `std.thread` starts operating
system threads. Async functions make tasks that an executor runs. The
compiler checks the data that moves between threads. In safe code, a data
race is a compile error.

## Starting a thread

`thread.spawn` takes a function that has no parameters and runs it on a new
thread. It returns a `Result`, because the operating system can refuse to
start a thread:

```synx
import std.thread;

int sum_to(int n) {
    int total = 0;
    for (i in 1..=n) {
        total += i;
    }
    return total;
}

main() {
    match thread.spawn(send once || sum_to(1000)) {
        Ok(handle) => {
            int result = handle.join();
            print("sum: {result}");
        }
        Err(error) => print("cannot start a thread: {error.os_code}"),
    }
}
```

```text
sum: 500500
```

`Ok` holds a `JoinHandle<int>`. `join` waits for the thread and returns the
value of its function. `join` takes `self`, so a handle can be joined only
once.

The closure is `send once`. `once` means that the closure runs one time and
can move its captured values. `send` means that its captured values can move
to another thread. `spawn` requires both. When you pass a closure directly to
`spawn`, the compiler adds `send` from the parameter type, so
`once || sum_to(1000)` also works. A named function has no captured values,
so `thread.spawn(compute)` works too.

## Moving values into a thread

A `send once` closure moves its captured values into the thread. Here the
thread owns `title` and returns a new `String`:

```synx
import std.thread;
import std.text;

main() {
    var title = String.from("Somewhere I Belong");
    var handle = thread.spawn(send once || text.to_ascii_uppercase(title));
    match handle {
        Ok(worker) => print(worker.join()),
        Err(_) => print("spawn failed"),
    }
}
```

```text
SOMEWHERE I BELONG
```

After the move, `main` cannot use `title`:

```synx
import std.thread;

main() {
    var title = String.from("Somewhere I Belong");
    var handle = thread.spawn(send once || {
        return "{title}";
    });
    print(title);
}
```

```text
example.snx:8:11: error[SNX0501]: use of moved value `title`; it was moved earlier
8 |     print(title);
  |           ^^^^^
1 error
```

## A dropped handle joins

Synx has no detached threads. When a `JoinHandle` goes out of scope, it
waits for its thread. The value that the thread returned is dropped:

```synx
import std.thread;

main() {
    {
        var worker = thread.spawn(send once || {
            thread.sleep_ms(50);
            print("worker done");
        });
    }
    print("after the scope");
}
```

```text
worker done
after the scope
```

`worker` holds a `Result` with the handle. At the end of the inner block the
handle drops, and `main` waits for the thread. So `worker done` always
prints first.

Each call to `print` writes its whole line at once. Lines from different
threads do not mix. Their order depends on the threads.

## Many threads

Keep the handles in a `Vec`, then join each one:

```synx
import std.thread;
import std.thread.JoinHandle;

int count_primes(int low, int high) {
    int count = 0;
    for (n in low..high) {
        if (n < 2) {
            continue;
        }
        bool prime = true;
        int d = 2;
        while (d * d <= n) {
            if (n % d == 0) {
                prime = false;
                break;
            }
            d += 1;
        }
        if (prime) {
            count += 1;
        }
    }
    return count;
}

main() {
    var handles = Vec<JoinHandle<int>>.new();
    for (part in 0..4) {
        int low = part * 25000;
        match thread.spawn(send once || count_primes(low, low + 25000)) {
            Ok(handle) => handles.push(handle),
            Err(_) => print("spawn failed"),
        }
    }
    int total = 0;
    while (!handles.is_empty()) {
        total += handles.pop().unwrap().join();
    }
    print("primes below 100000: {total}");
}
```

```text
primes below 100000: 9592
```

`low` is an `int`, so each closure captures its own copy.

## Send, Sync, and Owned

The prelude defines three marker traits. The compiler finds them for each
type from the types of its fields:

| Trait | Meaning |
|-------|---------|
| `Send` | A value of the type can move to another thread. |
| `Sync` | Several threads can use shared references to the value at the same time. |
| `Owned` | The value contains no references. |

Numbers, `bool`, and `char` have all three. `String`, `Vec<T>`,
`HashMap<K, V>`, and `Box<T>` have them when their element types have them.
A reference is never `Owned`. A thread can run longer than the function that
started it, so a `send` closure cannot capture a reference:

```synx
import std.thread;

main() {
    var title = String.from("Somewhere I Belong");
    var view = &const title;
    var handle = thread.spawn(send once || {
        return view.len();
    });
}
```

```text
example.snx:6:31: error: the type '&const String' does not implement 'Owned', required for send closure capture view
6 |     var handle = thread.spawn(send once || {
  |                               ^^^^^^^^^^^^^^
1 error
```

To share a value between threads, put it in an `Arc` from `std.sync`.

## Shared data: Arc and Mutex

`Arc<T>` gives shared ownership of one value. `clone` adds an owner. The last
owner to drop frees the value. `get` returns a shared reference, so the value
cannot change through an `Arc` alone. `Mutex<T>` permits changes. `lock`
returns a guard. `get_mut` on the guard gives a writable reference, and the
lock opens when the guard drops:

```synx
import std.thread;
import std.thread.JoinHandle;
import std.sync.{Arc, Mutex};

main() {
    var total = Arc.new(Mutex.new(0));
    var handles = Vec<JoinHandle<void>>.new();
    for (worker in 0..4) {
        var shared = total.clone();
        match thread.spawn(send once || {
            for (i in 0..1000) {
                var guard = shared.get().lock();
                *guard.get_mut() += 1;
            }
        }) {
            Ok(handle) => handles.push(handle),
            Err(_) => print("spawn failed"),
        }
    }
    handles.clear();
    var guard = total.get().lock();
    print("total: {*guard.get()}");
    print("owners: {total.strong_count()}");
}
```

```text
total: 4000
owners: 1
```

`handles.clear()` drops every handle, so `main` waits for all four threads.
Each thread drops its `Arc` when it ends, so one owner remains.

A Synx mutex owns the data that it protects. The only path to the data is
through a guard, so the program cannot forget the lock. A guard cannot move
to another thread. A mutex is not reentrant. A second `lock` on the same
thread while the first guard lives can deadlock.

`std.sync` also has `AtomicUsize`, `AtomicLong`, and `AtomicBool` for single
counters and flags, and `Condvar` for waiting on a condition.

The compiler rejects a type that is not safe to share. `Cell<T>` lets one
thread change a value through a shared reference. It is not `Sync`, so an
`Arc` that holds it is not `Send`:

```synx
import std.thread;
import std.sync.{Arc, Cell};

main() {
    var count = Arc.new(Cell.new(0));
    var shared = count.clone();
    var worker = thread.spawn(send once || {
        shared.get().set(5);
    });
}
```

```text
example.snx:7:31: error: the type 'std.sync.Arc<std.sync.Cell<int>>' does not implement 'Send', required for send closure capture shared
7 |     var worker = thread.spawn(send once || {
  |                               ^^^^^^^^^^^^^^
1 error
```

## Channels

`std.channel.channel<T>()` makes a queue between threads. It returns a pair.
Take the sending end with `sender()` and the receiving end with
`receiver()`. `recv` waits for the next value. It returns `None` when the
queue is empty and every sender has been dropped:

```synx
import std.thread;
import std.channel;

main() {
    var pair = channel.channel<String>();
    var sender = pair.sender();
    var receiver = pair.receiver();

    var producer = thread.spawn(send once || {
        sender.send(String.from("Take Me Back to Eden"));
        sender.send(String.from("Somewhere I Belong"));
        sender.send(String.from("Life Is Strange"));
    });

    loop {
        match receiver.recv() {
            Some(title) => print("now playing: {title}"),
            None => break,
        }
    }
    print("queue empty");
}
```

```text
now playing: Take Me Back to Eden
now playing: Somewhere I Belong
now playing: Life Is Strange
queue empty
```

The closure owns `sender`. When the thread ends, the closure drops it, and
`recv` returns `None`.

For several producers, clone the sender once for each thread. Then drop the
original with `std.mem.drop`. If `main` keeps it, `recv` waits forever:

```synx
import std.thread;
import std.thread.JoinHandle;
import std.channel;
import std.mem;

main() {
    var pair = channel.channel<int>();
    var sender = pair.sender();
    var receiver = pair.receiver();

    var handles = Vec<JoinHandle<void>>.new();
    for (id in 1..=3) {
        var out = sender.clone();
        match thread.spawn(send once || {
            for (step in 1..=10) {
                out.send(id * step);
            }
        }) {
            Ok(handle) => handles.push(handle),
            Err(_) => print("spawn failed"),
        }
    }
    mem.drop(sender);

    int count = 0;
    int sum = 0;
    loop {
        match receiver.recv() {
            Some(value) => {
                count += 1;
                sum += value;
            }
            None => break,
        }
    }
    print("{count} values, sum {sum}");
}
```

```text
30 values, sum 330
```

`send` returns `Err` with the value when the receiver has been dropped. A
channel can carry any type that is `Owned`.

## Async functions

An `async` function returns a task. The task does nothing until something
polls it. `await` waits for another task inside an async function.
`std.task.block_on` runs one task on the current thread and returns its
value:

```synx
import std.task.{block_on, yield_now};

async int add_sets(int a, int b) {
    await yield_now();
    return a + b;
}

async int total() {
    print("total starts");
    int first = await add_sets(3, 2);
    int second = await add_sets(first, 4);
    return second;
}

main() {
    Task<int> task = total();
    print("task created");
    print("sets: {block_on(task)}");
}
```

```text
task created
total starts
sets: 9
```

The written return type is the type of the value. A call to `total` gives a
`Task<int>`. `yield_now` lets the executor run other tasks. `main` cannot be
`async`. It starts the tasks with `block_on` or with an executor.

A dropped task that has not finished is cancelled. Its live values are
dropped.

## Running several tasks on one thread

`LocalExecutor` runs many `Task<void>` values on the current thread. Each
task runs until it reaches an `await` that is not ready. The executor then
runs the next task:

```synx
import std.task.{LocalExecutor, yield_now};

async void download(&const str name, int parts) {
    for (part in 1..=parts) {
        print("{name}: part {part} of {parts}");
        await yield_now();
    }
    print("{name}: done");
}

main() {
    var executor = LocalExecutor.new();
    executor.spawn(download("map", 2));
    executor.spawn(download("textures", 3));
    executor.run();
}
```

```text
map: part 1 of 2
textures: part 1 of 3
map: part 2 of 2
textures: part 2 of 3
map: done
textures: part 3 of 3
textures: done
```

`run` polls the ready tasks in the order of `spawn` and returns when all
tasks are done. Only one task runs at a time, so this order is always the
same. A `Task` stays on the thread that made it. It cannot move to another
thread.

## The worker pool

`async send` makes a task that can move between threads. A call returns a
`SendTask<T>`. `std.task.pool.Executor` runs these tasks on a fixed number of
worker threads. `spawn` returns a `Join<T>`, and `join` waits for the result:

```synx
import std.task.pool.{Executor, Join};
import std.task.yield_now;

async send long volume(long pounds, long reps, long sets) {
    await yield_now();
    return pounds * reps * sets;
}

enum PoolError { Start, Spawn, Join }

Result<long, PoolError> run() {
    var executor = match Executor.new(4) {
        Ok(pool) => pool,
        Err(_) => return Err(PoolError.Start),
    };
    var jobs = Vec<Join<long>>.new();
    long[3] weights = [135, 185, 225];
    for (pounds in weights) {
        match executor.spawn(volume(pounds, 5, 5)) {
            Ok(job) => jobs.push(job),
            Err(_) => return Err(PoolError.Spawn),
        }
    }
    long total = 0;
    while (!jobs.is_empty()) {
        match jobs.pop().unwrap().join() {
            Ok(value) => { total += value; }
            Err(_) => return Err(PoolError.Join),
        }
    }
    executor.shutdown();
    return Ok(total);
}

main() {
    match run() {
        Ok(total) => print("total volume: {total} lb"),
        Err(_) => print("pool error"),
    }
}
```

```text
total volume: 13625 lb
```

`Executor.new` fails when the worker count is 0 or a thread cannot start.
`spawn` fails after `shutdown`. `join` gives `Err` when the task was
cancelled. `shutdown` cancels the tasks that remain and joins the workers.

The parameters and the result of an `async send` function must be `Send`
and `Owned`, so a reference parameter is an error. A value that lives across
an `await` must be `Send` too. A mutex guard is not `Send`:

```synx
import std.sync.{Arc, Mutex};
import std.task.yield_now;

async send int read_total(Arc<Mutex<int>> total) {
    var guard = total.get().lock();
    await yield_now();
    return *guard.get();
}

main() {}
```

```text
example.snx:6:5: error[SNX0420]: cannot suspend a send task while `guard` of type 'std.sync.MutexGuard<int>' is retained; it does not implement Send
6 |     await yield_now();
  |     ^^^^^^^^^^^^^^^^^
1 error
```

Put the guard in an inner block that ends before the `await`.

## Tasks that start tasks

A task on a worker must not block its thread. Inside an async function,
`await` a `Join`. Do not call `join`. A `Spawner` from `executor.spawner()`
lets a task start more tasks. `block_on_send` runs one `SendTask` from
ordinary code:

```synx
import std.task.pool.{Executor, Spawner};
import std.task.{block_on_send, yield_now};

async send int scan(int sector) {
    await yield_now();
    return sector * 10;
}

async send int scan_all(Spawner spawner, int sectors) {
    int found = 0;
    for (sector in 1..=sectors) {
        match spawner.spawn(scan(sector)) {
            Ok(job) => match await job {
                Ok(value) => { found += value; }
                Err(_) => print("sector {sector} cancelled"),
            },
            Err(_) => print("pool closed"),
        }
    }
    return found;
}

main() {
    match Executor.new(2) {
        Ok(executor) => {
            int result = block_on_send(scan_all(executor.spawner(), 4));
            print("total: {result}");
        }
        Err(_) => print("cannot start the pool"),
    }
}
```

```text
total: 100
```

Some blocking calls on a worker thread stop the program. These are `join`,
`block_on`, `wait`, `shutdown`, and `Command.status`. The message names the
problem, for example:

```text
Synx panic: blocking join on async worker; await the handle
```

## Files in async code

`std.fs` functions block the thread that calls them. While a worker thread
waits for a file, it cannot run other tasks. `std.asyncio` runs file work on a
`BlockingPool`, which is a separate set of threads. Its functions take owned
arguments and return a `SendTask`:

```synx
import std.task.pool.{BlockingPool, BlockingSpawner};
import std.task.block_on_send;
import std.asyncio;

async send int save_and_count(BlockingSpawner io) {
    var saved = await asyncio.write_text(io.clone(), String.from("quickhacks.txt"),
        String.from("Short Circuit\nOverheat\nSynapse Burnout\n"));
    match saved {
        Ok(_) => {},
        Err(_) => return -1,
    }
    match await asyncio.read_text(io, String.from("quickhacks.txt")) {
        Ok(text) => return text.split("\n").len() as int - 1,
        Err(_) => return -1,
    }
}

main() {
    match BlockingPool.new(2) {
        Ok(io) => {
            int count = block_on_send(save_and_count(io.spawner()));
            print("quickhacks saved: {count}");
        }
        Err(_) => print("cannot start the I/O pool"),
    }
}
```

```text
quickhacks saved: 3
```

`asyncio` also has `read`, `write`, `read_line`, and `AsyncFile`. An error is
`asyncio.Error.Io` with the `IoError`, `Closed`, or `Cancelled`. To run other
blocking work, such as a `Command`, pass a `send once` closure to the
`spawn` method of a `BlockingSpawner` and `await` the `Join` that it returns.

The library reference has the full interfaces:
[std.thread](../../library/thread.md), [std.sync](../../library/sync.md),
[std.channel](../../library/channel.md), [std.task](../../library/task.md),
and [std.asyncio](../../library/asyncio.md).

## Next

[Chapter 15](15-project.md) builds a complete program: a training log.
