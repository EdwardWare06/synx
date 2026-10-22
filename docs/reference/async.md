# Async function and await expression

Creates a lazy task and suspends it while a future is pending.

## Syntax

```text
async return_type name(parameters) { body }
async send return_type name(parameters) { body }
await expression
```

## Explanation

Calling an ordinary async function constructs `Task<T>`, where `T` is the
written body result. Arguments move or copy into its fixed-address heap
frame. The body starts when polled. `await` is a prefix expression allowed
inside an async body. It uses `Future<T>.poll(&self, Waker)` and suspends
on `Poll.Pending`.

Ordinary Task values are local to one thread. Their references retain loans
of captured inputs. Moving a handle does not move its frame. The compiler
checks the logical body before converting it into frame operations and
records initialization for cancellation cleanup.

`async send` produces SendTask. Inputs and output must be Send and Owned;
values retained over an await must pass the sendability checks. Frame-local
borrows have a checked exception based on recorded origins. A MutexGuard
must not remain alive over an await in a send task.

## Example

```synx
import std.task.{block_on, yield_now};

async int answer() {
    await yield_now();
    return 42;
}

main() { print("{block_on(answer())}"); }
```

Output: `42`.

## Notes

Dropping an unfinished task cancels it and destroys its live values. Polling
a completed Task or polling it reentrantly panics. Async execution is
cooperative. `std.task.pool` provides the multithread executor; blocking
file operations run through its separate blocking pool.
