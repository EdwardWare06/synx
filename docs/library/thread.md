# std.thread

```synx
import std.thread;
import std.thread.{spawn, JoinHandle, ThreadError, sleep_ms};
```

This module starts operating system threads, waits for them to finish, and
pauses the current thread. For locks, atomics, and shared ownership, see
[std.sync](sync.md). For message queues between threads, see
[std.channel](channel.md).

A panic on any thread writes `Synx panic: ` and the message to standard error,
then ends the process with exit code 101. All other threads stop at once.
Destructors do not run on that path.

## Types

### ThreadError

```synx
struct ThreadError { pub int os_code; }
```

The error that [spawn](#spawn) returns when the operating system cannot start
a thread. On Windows, `os_code` is the C `errno` value, or `EAGAIN` if `errno`
is zero. On Linux and macOS, `os_code` is the error number that the `pthread`
call returned.

### JoinHandle

```synx
struct JoinHandle<T: Send + Owned>
```

An owned handle to a running thread. [spawn](#spawn) returns it. *T* is the
type of the value that the thread returns.

Dropping a handle blocks until the thread finishes, then drops the thread's
result. A thread cannot be detached. Release each resource that the thread
waits for, such as a channel sender, before its handle goes out of scope.

`JoinHandle<T>` is Send and Owned. A handle can move to another thread, and
that thread can join it.

### JoinHandle.join

```synx
T join(self)
```

Blocks until the thread finishes, then returns the value that its job
returned. The call consumes the handle.

Joining the handle of the current thread panics. On Windows the message is
`thread cannot join itself`. On other platforms the message is
`thread join failed`.

## Functions

### spawn

```synx
Result<JoinHandle<T>, ThreadError> spawn<T: Send + Owned>(fn once send() -> T job)
```

Starts a new thread that calls *job* one time. Returns `Ok` with a
[JoinHandle](#joinhandle), or `Err(ThreadError)` if the operating system
cannot create the thread. If the call fails, the job and its captured values
are dropped on the calling thread.

The type `fn once send() -> T` requires each captured value to be Send and
Owned. The compiler checks each capture. A captured reference fails the Owned
check. A closure literal takes the `send` qualifier from the parameter type,
so `once || ...` and `send once || ...` both work. A named function with no
parameters also works. The result type *T* must be Send and Owned.

Each thread reserves an 8 MB stack.

### yield_now

```synx
void yield_now()
```

Tells the operating system that it can run another thread now. The current
thread continues later. Inside an async task, use
[std.task.yield_now](task.md#yield_now).

### sleep_ms

```synx
void sleep_ms(usize milliseconds)
```

Blocks the current thread for *milliseconds* milliseconds. A long duration is
split into steps of one day each, so a large value is not cut short.

In a task on a [worker pool](task.md#executor), this call blocks the worker
thread. Use the timers in `std.time` to wait inside async code.

## Example

Sum a range on four threads:

```synx
import std.thread;

int sum_range(int from, int to) {
    int total = 0;
    for (i in from..to) { total += i; }
    return total;
}

int main() {
    var handles = Vec<thread.JoinHandle<int>>.new();
    for (part in 0..4) {
        int from = part * 250;
        match thread.spawn(send once || sum_range(from, from + 250)) {
            Ok(handle) => handles.push(handle),
            Err(error) => { print("spawn failed: {error.os_code}"); return 1; },
        }
    }
    int total = 0;
    while (!handles.is_empty()) {
        var handle = handles.pop().unwrap();
        int part = handle.join();
        print("part {handles.len()}: {part}");
        total += part;
    }
    print("total: {total}");
    return 0;
}
```

Output:

```text
part 3: 218625
part 2: 156125
part 1: 93625
part 0: 31125
total: 499500
```

A dropped handle waits for its thread. The worker line always comes first:

```synx
import std.thread;

int main() {
    var text = String.from("report");
    match thread.spawn(send once || {
        thread.sleep_ms(50);
        print("worker wrote {text}");
    }) {
        Ok(handle) => {},
        Err(_) => return 1,
    }
    print("main continues");
    return 0;
}
```

Output:

```text
worker wrote report
main continues
```
