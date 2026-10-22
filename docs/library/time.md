# std.time

```synx
import std.time;
import std.time.{TimerService, TimerSpawner, Sleep, Timeout, TimerError, TimeoutError, now_ms};
```

This module reads a monotonic clock and gives timers for async tasks. A
`TimerService` runs one reactor thread. The thread keeps a list of
deadlines and sleeps until the earliest one. When a deadline passes, the
thread wakes the task that waits on it. Task code never runs on the reactor
thread. For `async`, `await`, and futures, see
[Async function and await expression](../reference/async.md).

The clock has a resolution of one millisecond. Do not expect a timer to be
more accurate than that.

## Types

### TimerError

```synx
enum TimerError { Closed, Cancelled }
```

The error of a [Sleep](#sleep). `Closed` means that the timer service shut
down. `Cancelled` means that [Sleep.cancel](#sleepcancel) was called.

### TimeoutError

```synx
enum TimeoutError { Elapsed, TimerClosed }
```

The error of a [Timeout](#timeout). `Elapsed` means that the time ran out
before the task finished. `TimerClosed` means that the sleep of the timeout
ended with a `TimerError`.

### TimerService

```synx
struct TimerService
```

Owns the reactor thread and the deadline list. The fields are private.
When a `TimerService` is dropped, it calls [shutdown](#timerserviceshutdown).

### TimerService.new

```synx
Result<TimerService, thread.ThreadError> new()
```

Starts the reactor thread and returns the service.

Errors:

- `thread.ThreadError` from `std.thread` if the thread cannot start.

### TimerService.spawner

```synx
TimerSpawner spawner(&const self)
```

Returns a [TimerSpawner](#timerspawner) that shares the deadline list of
this service.

### TimerService.sleep

```synx
Sleep sleep(&const self, usize milliseconds)
```

Same as [TimerSpawner.sleep](#timerspawnersleep) on a new spawner.

### TimerService.timeout

```synx
Timeout<T> timeout<T: Send + Owned>(&const self, SendTask<T> task, usize milliseconds)
```

Same as [TimerSpawner.timeout](#timerspawnertimeout) on a new spawner.

### TimerService.shutdown

```synx
void shutdown(&self)
```

Closes the deadline list, stops the reactor thread, and waits for the
thread to end. All pending sleeps complete with `Err(TimerError.Closed)`.
A sleep that is created after the shutdown completes at once with the same
error. A second call does nothing.

Panics with `blocking join on async worker; await the handle` if it is
called on an async executor worker while the reactor thread runs. Drop
calls `shutdown`, so the drop of a running `TimerService` on an async
worker also panics.

### TimerSpawner

```synx
struct TimerSpawner
```

A handle to the deadline list of a `TimerService`. The fields are private.
A spawner can move into an `async send` task. A spawner stays valid after
the service shuts down. Its sleeps then complete with
`Err(TimerError.Closed)`.

### TimerSpawner.clone

```synx
TimerSpawner clone(&const self)
```

Returns a second handle to the same deadline list.

### TimerSpawner.sleep

```synx
Sleep sleep(&const self, usize milliseconds)
```

Returns a [Sleep](#sleep) that completes after *milliseconds*. The deadline
is set when `sleep` is called: it is `now_ms()` plus *milliseconds*. A
deadline that is larger than the largest `usize` is set to the largest
`usize`.

### TimerSpawner.timeout

```synx
Timeout<T> timeout<T: Send + Owned>(&const self, SendTask<T> task, usize milliseconds)
```

Returns a [Timeout](#timeout) that runs *task* with a time limit of
*milliseconds*. The time limit starts when `timeout` is called.

### Sleep

```synx
struct Sleep
```

A future that completes at a deadline. `Sleep` implements
`Future<Result<void, TimerError>>`. Use `await` to wait for it.

The result is:

- `Ok(())` when the deadline has passed.
- `Err(TimerError.Closed)` when the service shut down.
- `Err(TimerError.Cancelled)` after [cancel](#sleepcancel).

If the service shut down before the cancel, the result is `Closed`.

A poll after the `Sleep` has completed panics with
`timer polled after completion`.

When an unfinished `Sleep` is dropped, it cancels itself and its deadline
is removed from the list.

### Sleep.cancel

```synx
void cancel(&self)
```

Cancels the sleep. The reactor removes its deadline. If a task waits on the
sleep, the task wakes, and the next poll returns
`Err(TimerError.Cancelled)`.

### Timeout

```synx
struct Timeout<T: Send + Owned>
```

A future that runs a task with a time limit. `Timeout<T>` implements
`Future<Result<T, TimeoutError>>`. The fields are private.

Each poll polls the task first. If the task is not ready, it polls the
sleep. The result is:

- `Ok(value)` if the task finishes. The sleep is cancelled.
- `Err(TimeoutError.Elapsed)` if the time runs out first. The task is
  dropped, which cancels it.
- `Err(TimeoutError.TimerClosed)` if the sleep ends with a `TimerError`.
  The task is dropped.

Because the task is polled first, a task that finishes on its first poll
gives `Ok` also with a time limit of zero.

A poll after the `Timeout` has completed panics with
`timeout polled after completion`.

## Functions

### now_ms

```synx
usize now_ms()
```

Returns the time of a monotonic clock in milliseconds. The clock does not
go back when the system time changes. The start point of the clock is not
specified. Use the difference between two values to measure time.

On Windows, the clock is `QueryPerformanceCounter`. On Linux and macOS, the
clock is `CLOCK_MONOTONIC`.

Panics with `monotonic clock failed` if the clock cannot be read.

## Example

```synx
import std.task.block_on;
import std.time;
import std.time.{TimerService, TimerSpawner, TimerError, TimeoutError, Sleep};

async send int slow(TimerSpawner timers) {
    var finished = await timers.sleep(1000);
    return 1;
}

async send int quick() { return 2; }

async void run(TimerSpawner timers) {
    usize start = time.now_ms();
    match await timers.sleep(20) {
        Ok(_) => print("slept at least 20 ms: {time.now_ms() - start >= 20}"),
        Err(_) => print("sleep failed"),
    }
    match await timers.timeout(quick(), 50) {
        Ok(value) => print("quick finished with {value}"),
        Err(_) => print("quick timed out"),
    }
    match await timers.timeout(slow(timers.clone()), 50) {
        Ok(value) => print("slow finished with {value}"),
        Err(TimeoutError.Elapsed) => print("slow timed out"),
        Err(TimeoutError.TimerClosed) => print("timer closed"),
    }
}

async Result<void, TimerError> wait(Sleep sleep) { return await sleep; }

int main() {
    var timers = match TimerService.new() {
        Ok(service) => service,
        Err(_) => { print("cannot start the timer thread"); return 1; },
    };
    block_on(run(timers.spawner()));
    timers.shutdown();
    match block_on(wait(timers.sleep(10))) {
        Ok(_) => print("late sleep finished"),
        Err(TimerError.Closed) => print("late sleep: Closed"),
        Err(TimerError.Cancelled) => print("late sleep: Cancelled"),
    }
    return 0;
}
```

Output:

```text
slept at least 20 ms: true
quick finished with 2
slow timed out
late sleep: Closed
```
