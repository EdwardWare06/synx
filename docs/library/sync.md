# std.sync

```synx
import std.sync;
import std.sync.{Arc, Mutex, Condvar, Cell, AtomicUsize, AtomicLong, AtomicBool};
```

This module holds atomic integers and booleans, the shared owner `Arc`, the
lock `Mutex` and its guard, the condition variable `Condvar`, and `Cell` for
changes to a value on one thread. All atomic operations use sequential
consistency. There is no ordering argument.

The marker traits `Send`, `Sync`, and `Owned` are defined in the
[prelude](prelude.md). The compiler derives them for a struct from its fields.
The types in this module state their own rules below.

## Types

### AtomicUsize

```synx
struct AtomicUsize
```

An unsigned 64-bit integer that several threads can read and change at the
same time. All methods take `&const self`, so a shared reference, for example
from an [Arc](#arc), is sufficient. Arithmetic wraps on overflow and
underflow.

The type has a destructor, so assignment moves the value. `AtomicUsize` is
Send, Sync, and Owned.

### AtomicUsize.new

```synx
AtomicUsize new(usize value)
```

Returns an atomic that holds *value*.

### AtomicUsize.load

```synx
usize load(&const self)
```

Returns the current value.

### AtomicUsize.store

```synx
void store(&const self, usize value)
```

Replaces the current value with *value*.

### AtomicUsize.swap

```synx
usize swap(&const self, usize value)
```

Replaces the current value with *value* and returns the previous value.

### AtomicUsize.fetch_add

```synx
usize fetch_add(&const self, usize value)
```

Adds *value* and returns the previous value. The sum wraps at 2^64.

### AtomicUsize.fetch_sub

```synx
usize fetch_sub(&const self, usize value)
```

Subtracts *value* and returns the previous value. The result wraps below
zero.

### AtomicUsize.compare_exchange

```synx
Result<usize, usize> compare_exchange(&const self, usize expected, usize next)
```

If the current value equals *expected*, stores *next* and returns
`Ok(expected)`. Otherwise the value does not change, and the call returns
`Err` with the current value.

### AtomicLong

```synx
struct AtomicLong
```

A signed 64-bit integer with the same operations as
[AtomicUsize](#atomicusize). It stores the value in an `AtomicUsize`.
Arithmetic wraps: adding 1 to the largest `long` gives the smallest `long`.
`AtomicLong` is Send, Sync, and Owned.

### AtomicLong.new

```synx
AtomicLong new(long value)
```

Returns an atomic that holds *value*.

### AtomicLong.load

```synx
long load(&const self)
```

Returns the current value.

### AtomicLong.store

```synx
void store(&const self, long value)
```

Replaces the current value with *value*.

### AtomicLong.swap

```synx
long swap(&const self, long value)
```

Replaces the current value with *value* and returns the previous value.

### AtomicLong.fetch_add

```synx
long fetch_add(&const self, long value)
```

Adds *value* and returns the previous value.

### AtomicLong.fetch_sub

```synx
long fetch_sub(&const self, long value)
```

Subtracts *value* and returns the previous value.

### AtomicLong.compare_exchange

```synx
Result<long, long> compare_exchange(&const self, long expected, long next)
```

If the current value equals *expected*, stores *next* and returns
`Ok(expected)`. Otherwise the value does not change, and the call returns
`Err` with the current value.

### AtomicBool

```synx
struct AtomicBool
```

A boolean that several threads can read and change at the same time. It
stores the value in an `AtomicUsize`. `AtomicBool` is Send, Sync, and Owned.

### AtomicBool.new

```synx
AtomicBool new(bool value)
```

Returns an atomic that holds *value*.

### AtomicBool.load

```synx
bool load(&const self)
```

Returns the current value.

### AtomicBool.store

```synx
void store(&const self, bool value)
```

Replaces the current value with *value*.

### AtomicBool.swap

```synx
bool swap(&const self, bool value)
```

Replaces the current value with *value* and returns the previous value.

### AtomicBool.compare_exchange

```synx
Result<bool, bool> compare_exchange(&const self, bool expected, bool next)
```

If the current value equals *expected*, stores *next* and returns
`Ok(expected)`. Otherwise the value does not change, and the call returns
`Err` with the current value.

### Arc

```synx
struct Arc<T: Owned>
```

Shared ownership of one heap value through an atomic reference count. Each
clone is one more owner. When the last owner is dropped, it drops the value
and frees the memory.

An `Arc` gives only shared access to its value. To change the value from
several owners, put a [Mutex](#mutex), an atomic, or a [Cell](#cell) inside
the `Arc`.

*T* must be Owned, so an `Arc` cannot hold a reference. `Arc<T>` is Send and
Sync when *T* is Send and Sync. It is always Owned.

Owners that form a cycle are never freed. There is no weak reference type.

### Arc.new

```synx
Arc<T> new(T value)
```

Moves *value* to the heap and returns its first owner. The count starts at 1.

### Arc.clone

```synx
Arc<T> clone(&const self)
```

Adds one to the count and returns a new owner of the same value.

Panics with `Arc reference count overflow` if the count was already
9223372036854775807 or more.

### Arc.get

```synx
&const T get(&const self)
```

Returns a shared reference to the value. The reference borrows this owner.

### Arc.strong_count

```synx
usize strong_count(&const self)
```

Returns the current number of owners. Other threads can change the count at
any time.

### Mutex

```synx
struct Mutex<T>
```

A lock that protects one value of type *T*. The value is stored on the heap.
Call [lock](#mutexlock) or [try_lock](#mutextry_lock) to get a
[MutexGuard](#mutexguard), and read or change the value through the guard.

The mutex is not recursive. Do not lock it again on a thread that holds one
of its guards: the second call can deadlock. A panic ends the process, so the
mutex has no poisoned state.

Dropping the mutex drops the value. `Mutex<T>` is Send and Sync when *T* is
Send. It is Owned when *T* is Owned.

### Mutex.new

```synx
Mutex<T> new(T value)
```

Returns an unlocked mutex that owns *value*.

### Mutex.lock

```synx
MutexGuard<T> lock(&const self)
```

Blocks until the lock is free, takes it, and returns a guard.

### Mutex.try_lock

```synx
MutexGuard<T>? try_lock(&const self)
```

Takes the lock and returns `Some(guard)` if the lock is free. Returns `None`
at once if another guard holds the lock.

### MutexGuard

```synx
struct MutexGuard<T>
```

Access to the value of a locked [Mutex](#mutex). Dropping the guard unlocks
the mutex.

The guard borrows its mutex, so it cannot outlive the mutex. It is not Send
and not Owned, so it cannot move to another thread. An `async send` function
cannot keep a guard alive across an `await`. The compiler rejects this with
SNX0420. Drop the guard before the `await`.

### MutexGuard.get

```synx
&const T get(&const self)
```

Returns a shared reference to the value. The reference borrows the guard.

### MutexGuard.get_mut

```synx
&T get_mut(&self)
```

Returns a writable reference to the value. The reference borrows the guard.

### Condvar

```synx
struct Condvar
```

A condition variable. Threads wait on it for a change to a value that a
[Mutex](#mutex) protects.

The first wait binds the condition variable to the mutex of its guard. A
later wait with a guard of a different mutex panics with
`condition variable used with different mutexes`.

A wait can return without a notification. Test the condition in a loop.

`Condvar` is Send, Sync, and Owned.

### Condvar.new

```synx
Condvar new()
```

Returns a condition variable that is not bound to a mutex.

### Condvar.wait

```synx
void wait<T>(&const self, &MutexGuard<T> guard)
```

Unlocks the mutex of *guard* and blocks until a notification arrives. Then
locks the mutex again and returns. The guard stays valid.

### Condvar.wait_for

```synx
bool wait_for<T>(&const self, &MutexGuard<T> guard, usize milliseconds)
```

Like [wait](#condvarwait), with a time limit. Returns `true` if the thread
woke before the limit, and `false` if the time ran out. In both cases the
mutex is locked again before the call returns. A limit above 86400000
milliseconds (one day) is reduced to one day.

### Condvar.notify_one

```synx
void notify_one(&const self)
```

Wakes one thread that waits on this condition variable, if there is one.

### Condvar.notify_all

```synx
void notify_all(&const self)
```

Wakes all threads that wait on this condition variable.

### Cell

```synx
struct Cell<T: Copy + Owned>
```

A heap slot for a `Copy` value. The value can change through a shared
reference. All methods take `&const self`.

`Cell<T>` is Send when *T* is Send, so a cell can move to another thread. It
is never Sync, so `Arc<Cell<T>>` cannot move to another thread. To share a
changing value between threads, use a [Mutex](#mutex) or an atomic. `Cell<T>`
is Owned.

### Cell.new

```synx
Cell<T> new(T value)
```

Returns a cell that holds *value*.

### Cell.get

```synx
T get(&const self)
```

Returns a copy of the value.

### Cell.set

```synx
void set(&const self, T value)
```

Replaces the value with *value*.

### Cell.replace

```synx
T replace(&const self, T value)
```

Replaces the value with *value* and returns the previous value.

## Example

Four threads update a shared counter and a locked vector:

```synx
import std.thread;
import std.sync.{Arc, AtomicUsize, Mutex};

int main() {
    var hits = Arc.new(AtomicUsize.new(0));
    var words = Arc.new(Mutex.new(Vec<String>.new()));
    var handles = Vec<thread.JoinHandle<void>>.new();
    for (id in 0..4) {
        var hits_ref = hits.clone();
        var words_ref = words.clone();
        match thread.spawn(send once || {
            for (i in 0..1000) { hits_ref.get().fetch_add(1); }
            var guard = words_ref.get().lock();
            guard.get_mut().push("worker {id}");
        }) {
            Ok(handle) => handles.push(handle),
            Err(_) => return 1,
        }
    }
    while (!handles.is_empty()) { handles.pop().unwrap().join(); }
    print("hits: {hits.get().load()}");
    {
        var guard = words.get().lock();
        print("entries: {guard.get().len()}");
    }
    print("owners: {words.strong_count()}");
    return 0;
}
```

Output:

```text
hits: 4000
entries: 4
owners: 1
```

A thread waits on a condition variable until another thread sets a value:

```synx
import std.thread;
import std.sync.{Arc, Mutex, Condvar, Cell};

struct Slot { Mutex<int?> value; Condvar filled; }

int main() {
    var slot = Arc.new(Slot { value: Mutex.new(None), filled: Condvar.new() });
    var writer = slot.clone();
    match thread.spawn(send once || {
        var guard = writer.get().value.lock();
        *guard.get_mut() = Some(42);
        writer.get().filled.notify_one();
    }) {
        Ok(handle) => {
            var shared = slot.get();
            var guard = shared.value.lock();
            while (guard.get().is_none()) { shared.filled.wait(&guard); }
            match guard.get() { Some(v) => print("received {*v}"), None => {} }
        },
        Err(_) => return 1,
    }
    var count = Cell.new(1);
    print("old {count.replace(5)}, new {count.get()}");
    return 0;
}
```

Output:

```text
received 42
old 1, new 5
```
