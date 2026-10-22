# std.task

```synx
import std.task.{block_on, block_on_send, yield_now, LocalExecutor};
import std.task.pool.{Executor, Spawner, Join, BlockingPool, BlockingSpawner};
```

This module runs async tasks. `std.task` holds the drivers that run tasks on
the calling thread: [block_on](#block_on), [block_on_send](#block_on_send),
and [LocalExecutor](#localexecutor). `std.task.pool` holds a multithread
worker pool, [Executor](#executor), and a pool for blocking calls,
[BlockingPool](#blockingpool). For async file and input operations, see
[std.asyncio](asyncio.md).

The task and future types are defined in the [prelude](prelude.md), so they
need no import. `std.task` re-exports `Task`, `SendTask`, `Future`, `Poll`,
and `Waker`. `std.task.pool` re-exports `SendTask`.

A call to an `async` function returns a `Task<T>`. A call to an `async send`
function returns a `SendTask<T>`. The body does not run until the task is
polled. `Task` values stay on the thread that created them. `SendTask`
values can move to other threads and run on the worker pool.

Some calls block the current thread: `block_on`, `block_on_send`,
`LocalExecutor.run`, `Waker.wait`, `Join.join`, `Executor.wait`, and
`Executor.shutdown`. On a worker thread of an `Executor`, each of these calls
panics with `blocking join on async worker; await the handle`. Use `await`
there. On a `BlockingPool` worker, they are allowed. A `join`, `wait`, or
`shutdown` call on a worker of the same pool panics with
`executor cannot block its own worker`.

## Prelude types

### Poll

```synx
enum Poll<T> { Pending, Ready(T) }
```

The result of one poll. `Ready` holds the output of a finished future.
`Pending` means the future is not finished.

### Future

```synx
trait Future<T> { Poll<T> poll(&self, Waker wake); }
```

A value that produces a *T* after one or more polls. `await` polls a future
until it returns `Ready`. `await` accepts an owned future, a writable
reference `&F` to a future, or a `&dyn Future<T>`. `Task<T>`, `SendTask<T>`,
`YieldNow`, and `Join<T>` implement `Future`. User types can implement it.

A `poll` that returns `Pending` must arrange for *wake* to be woken when the
future can make progress. It can keep *wake* for later. On each poll, keep the
newest handle, because a task can move to a different executor. If no copy of
the handle remains, the local drivers panic with
`local executor has no ready task`. If a copy remains and nothing wakes it,
the driver waits forever.

A `poll` method must return promptly. It runs on the thread of the driver and
blocks other tasks on that thread while it runs.

### Wake

```synx
trait Wake { void wake(&const self); }
```

A callback for [Waker.from](#wakerfrom). Several threads can call `wake` at
the same time through shared references.

### Waker

```synx
struct Waker
```

A reference-counted wake handle. It holds a ready flag and an optional
[Wake](#wake) callback. Clones share the flag and the callback. A handle stays
valid after its task or executor is dropped. `Waker` is Send, Sync, and
Owned, so a task can give a copy to another thread to wake it later.

### Waker.new

```synx
Waker new()
```

Returns a handle with no callback. The ready flag starts set.

### Waker.from

```synx
Waker from<W: Wake + Send + Sync + Owned>(W value)
```

Returns a handle that calls `value.wake()` on each [wake](#wakerwake). The
ready flag starts set. *value* is dropped when the last handle is dropped.

### Waker.clone

```synx
Waker clone(&const self)
```

Returns a new handle that shares the flag and callback.

Panics with `too many waker references` if the count of handles was already
9223372036854775807 or more.

### Waker.wake

```synx
void wake(&const self)
```

Sets the ready flag, wakes the threads that wait in [wait](#wakerwait), and
then calls the callback, if there is one.

### Waker.take_ready

```synx
bool take_ready(&const self)
```

Returns `true` if the ready flag was set, and clears it.

### Waker.wait

```synx
void wait(&const self)
```

Blocks until the ready flag is set. The flag stays set. See the blocking rule
at the top of this page.

### Waker.is_retained

```synx
bool is_retained(&const self)
```

Returns `true` if more than one handle shares this state.

### Task

```synx
struct Task<T>
```

The task that an `async` function returns. Only the compiler creates `Task`
values. The task frame is on the heap at a fixed address, so the task can move
while its body holds references to its own locals. `Task<T>` implements
`Future<T>`.

`Task<T>` is not Send and not Owned. It cannot move to another thread, and it
can hold borrowed arguments. Run it with [block_on](#block_on), a
[LocalExecutor](#localexecutor), or `await` in another async function.

Dropping a task cancels it. Cancellation drops each live value in the frame
one time. A task that was never polled drops its arguments.

Panics with `async task is already being polled` if a poll starts while
another poll of the same task runs. Panics with
`async task polled after completion` if it is polled after it returned
`Ready`.

### SendTask

```synx
struct SendTask<T: Send + Owned>
```

The task that an `async send` function returns. It has the same poll,
cancel, and panic rules as [Task](#task). `SendTask<T>` is Send and Owned, so
it can move to another thread and run on an [Executor](#executor).

The compiler checks each `async send` function:

- Each parameter must be Send and Owned. A reference parameter and
  `&const self` are rejected. Take `self` by value.
- The output type must be Send and Owned.
- Each local that is alive across an `await` must be Send. A reference to a
  local of the same task is allowed. A `MutexGuard`, raw pointer, `dyn`
  value, local `Task`, or plain closure alive across an `await` is rejected
  with SNX0420.

## Types

### YieldNow

```synx
struct YieldNow
```

The future that [yield_now](#yield_now) returns. The first poll wakes the
task and returns `Pending`. The second poll returns `Ready`. `YieldNow`
implements `Future<void>`.

### LocalExecutor

```synx
struct LocalExecutor
```

Runs several `Task<void>` values on the calling thread. On each pass, it polls
each ready task in spawn order. `LocalExecutor` is not Send, so it stays on
the thread that created it.

Dropping the executor drops its remaining tasks, which cancels them.

### LocalExecutor.new

```synx
LocalExecutor new()
```

Returns an executor with no tasks.

### LocalExecutor.spawn

```synx
void spawn(&self, Task<void> task)
```

Adds *task* to the executor. The task does not run until
[run](#localexecutorrun). Each task gets its own [Waker](#waker). Its flag
starts set, so the first pass of `run` polls the task.

### LocalExecutor.run

```synx
void run(&self)
```

Polls the tasks until all of them are finished, then returns. A finished task
is dropped at once. After `run` returns, the executor has no tasks and can be
used again.

If no task is ready and a copy of a pending task's waker still exists, `run`
blocks until a waker is woken. A wake from another thread also ends the
wait.

Panics with `local executor has no ready task` if no task is ready and no
waker of a pending task has another copy. See the blocking rule at the top of
this page.

## Functions

### block_on

```synx
T block_on<T>(Task<T> task)
```

Runs *task* on the calling thread and returns its output. The first poll
happens at once. After each `Pending`, the call blocks until the task's waker
is woken, then polls again.

Panics with `local executor has no ready task` if the task returns `Pending`
and no copy of its waker remains. See the blocking rule at the top of this
page.

### block_on_send

```synx
T block_on_send<T: Send + Owned>(SendTask<T> task)
```

Like [block_on](#block_on), for a `SendTask`.

### yield_now

```synx
YieldNow yield_now()
```

Returns a future that gives other tasks one turn. `await yield_now()` returns
after one more pass of the driver. For OS threads, see
[std.thread.yield_now](thread.md#yield_now).

## Pool types

These types are in `std.task.pool`.

### JoinError

```synx
enum JoinError { Cancelled }
```

The error that a [Join](#join) gives when its task was cancelled or dropped
before it produced a value.

### SpawnError

```synx
enum SpawnError { Closed }
```

The error that `spawn` returns after the pool was shut down. The task is
dropped.

### ExecutorError

```synx
enum ExecutorError { InvalidWorkers, Thread(thread.ThreadError) }
```

The error from [Executor.new](#executornew) and
[BlockingPool.new](#blockingpoolnew). `InvalidWorkers` means the worker count
was zero. `Thread` holds the [ThreadError](thread.md#threaderror) from a
worker that could not start. The workers that did start are stopped and
joined.

### Executor

```synx
struct Executor
```

A fixed number of worker threads that run `SendTask` values. All workers
share one ready queue, in first-in, first-out order. One worker at a time
polls a given task, outside all locks. If the task is woken during a poll, it
goes back on the queue after the poll.

Dropping the executor calls [shutdown](#executorshutdown).

### Executor.new

```synx
Result<Executor, ExecutorError> new(usize count)
```

Starts *count* worker threads and returns the executor.

Errors:

- `ExecutorError.InvalidWorkers` if *count* is zero.
- `ExecutorError.Thread(error)` if a worker thread could not start.

### Executor.spawner

```synx
Spawner spawner(&const self)
```

Returns a [Spawner](#spawner) for this executor.

### Executor.spawn

```synx
Result<Join<T>, SpawnError> spawn<T: Send + Owned>(&const self, SendTask<T> task)
```

Puts *task* on the ready queue and returns a [Join](#join) for its result.
Returns `Err(SpawnError.Closed)` after [shutdown](#executorshutdown).

Dropping the returned `Join` cancels the task. Keep the `Join` until the task
is finished.

### Executor.wait

```synx
void wait(&const self)
```

Blocks until every spawned task is finished or cancelled. See the blocking
rule at the top of this page.

### Executor.shutdown

```synx
void shutdown(&self)
```

Closes the pool, cancels every task, and joins all worker threads. After
this call, `spawn` returns `Err(SpawnError.Closed)`. A task that is in a poll
finishes that poll first. A cancelled task drops its live values on a worker
thread. A second call returns at once. See the blocking rule at the top of
this page.

### Spawner

```synx
struct Spawner
```

A handle that adds tasks to an [Executor](#executor). It is Send and Owned, so
a task can take a `Spawner` as a parameter and spawn more tasks. A spawner
keeps the shared queue alive after the executor is dropped. Then `spawn`
returns `Err(SpawnError.Closed)`.

### Spawner.clone

```synx
Spawner clone(&const self)
```

Returns a new handle to the same executor.

### Spawner.spawn

```synx
Result<Join<T>, SpawnError> spawn<T: Send + Owned>(&const self, SendTask<T> task)
```

Same as [Executor.spawn](#executorspawn).

### Join

```synx
struct Join<T: Send + Owned>
```

The handle to the result of a pool task. `Join<T>` implements
`Future<Result<T, JoinError>>`. Inside a task, `await` it. On a thread
outside the pool, call [join](#joinjoin). `Join<T>` is Send and Owned.

Dropping a `Join` cancels its task. If the task is finished, the drop has no
effect.

### Join.cancel

```synx
void cancel(&const self)
```

Asks the pool to cancel the task. A queued or idle task is dropped the next
time a worker takes it. A task that is in a poll finishes that poll first. If
that poll returns `Ready`, the result is kept. Otherwise the task is dropped,
and the `Join` gives `Err(JoinError.Cancelled)`. A finished task does not
change.

### Join.join

```synx
Result<T, JoinError> join(self)
```

Blocks until the task is finished or cancelled. Returns `Ok(value)` or
`Err(JoinError.Cancelled)`. See the blocking rule at the top of this page.

Panics with `async join result already consumed` if an earlier `await` of
this handle took the result.

### BlockingPool

```synx
struct BlockingPool
```

A fixed number of worker threads for functions that block, such as file and
input calls. Each job is a closure. The workers are marked as blocking
workers, so a job can call `block_on`, `Waker.wait`, and `join` on a
different pool. Jobs that wait on each other need enough workers.

Dropping the pool calls [shutdown](#blockingpoolshutdown).

### BlockingPool.new

```synx
Result<BlockingPool, ExecutorError> new(usize count)
```

Starts *count* worker threads. The errors are the same as for
[Executor.new](#executornew).

### BlockingPool.spawner

```synx
BlockingSpawner spawner(&const self)
```

Returns a [BlockingSpawner](#blockingspawner) for this pool.

### BlockingPool.spawn

```synx
Result<Join<T>, SpawnError> spawn<T: Send + Owned>(&const self, fn once send() -> T job)
```

Queues *job* to run on a worker and returns a [Join](#join) for its result.
Returns `Err(SpawnError.Closed)` after shutdown. Dropping the `Join` before
the job starts cancels the job.

### BlockingPool.wait

```synx
void wait(&const self)
```

Blocks until every queued job is finished or cancelled.

### BlockingPool.shutdown

```synx
void shutdown(&self)
```

Closes the pool, cancels the jobs that have not started, and joins the
workers. A job that is running finishes first.

### BlockingSpawner

```synx
struct BlockingSpawner
```

A handle that adds jobs to a [BlockingPool](#blockingpool). It is Send and
Owned. The functions in [std.asyncio](asyncio.md) take one.

### BlockingSpawner.clone

```synx
BlockingSpawner clone(&const self)
```

Returns a new handle to the same pool.

### BlockingSpawner.spawn

```synx
Result<Join<T>, SpawnError> spawn<T: Send + Owned>(&const self, fn once send() -> T job)
```

Same as [BlockingPool.spawn](#blockingpoolspawn).

## Example

Run tasks on the calling thread:

```synx
import std.task.{block_on, yield_now, LocalExecutor};

async int add(int a, int b) {
    await yield_now();
    return a + b;
}

async void worker(String name, int steps) {
    for (i in 0..steps) {
        print("{name} step {i}");
        await yield_now();
    }
    print("{name} done");
}

struct Countdown { int remaining; }
impl Future<int> for Countdown {
    Poll<int> poll(&self, Waker wake) {
        if (self.remaining == 0) { return Poll.Ready(7); }
        self.remaining -= 1;
        wake.wake();
        return Poll.Pending;
    }
}

async int counted() { return await Countdown { remaining: 3 }; }

int main() {
    print("sum {block_on(add(2, 3))}");
    print("countdown {block_on(counted())}");
    var executor = LocalExecutor.new();
    executor.spawn(worker(String.from("a"), 2));
    executor.spawn(worker(String.from("b"), 1));
    executor.run();
    return 0;
}
```

Output:

```text
sum 5
countdown 7
a step 0
b step 0
a step 1
b done
a done
```

A pool task spawns child tasks and awaits their results:

```synx
import std.task.pool.{Executor, Spawner, Join};
import std.task.{yield_now, block_on_send};
import std.sync.{Arc, AtomicUsize};

async send int square(int n, Arc<AtomicUsize> polls) {
    await yield_now();
    polls.get().fetch_add(1);
    return n * n;
}

async send int sum_squares(Spawner spawner, int count, Arc<AtomicUsize> polls) {
    var joins = Vec<Join<int>>.new();
    for (i in 1..count + 1) {
        match spawner.spawn(square(i, polls.clone())) {
            Ok(join) => joins.push(join),
            Err(_) => return -1,
        }
    }
    int total = 0;
    while (!joins.is_empty()) {
        match await joins.pop().unwrap() {
            Ok(value) => { total += value; },
            Err(_) => return -1,
        }
    }
    return total;
}

int main() {
    var polls = Arc.new(AtomicUsize.new(0));
    match Executor.new(4) {
        Ok(executor) => {
            match executor.spawn(sum_squares(executor.spawner(), 10, polls.clone())) {
                Ok(join) => match join.join() {
                    Ok(total) => print("sum of squares: {total}"),
                    Err(_) => print("cancelled"),
                },
                Err(_) => print("pool closed"),
            }
            executor.wait();
            executor.shutdown();
            match executor.spawn(square(1, polls.clone())) {
                Ok(_) => print("accepted"),
                Err(_) => print("spawn after shutdown: closed"),
            }
        },
        Err(_) => return 1,
    }
    print("tasks run: {polls.get().load()}");
    print("on this thread: {block_on_send(square(9, polls.clone()))}");
    return 0;
}
```

Output:

```text
sum of squares: 385
spawn after shutdown: closed
tasks run: 10
on this thread: 81
```
