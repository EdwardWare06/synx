# std.channel

```synx
import std.channel;
import std.channel.{channel, Sender, Receiver};
```

This module holds an unbounded queue that moves values between threads. A
channel has one [Receiver](#receiver) and one or more [Senders](#sender).
Values arrive in the order that `send` calls add them to the queue. The queue
is built from a [Mutex](sync.md#mutex) and a [Condvar](sync.md#condvar).

The payload type *T* must be Owned, so a channel cannot carry references.
`Sender<T>` and `Receiver<T>` are Send and Sync when *T* is Send. They are
always Owned.

## Types

### ChannelPair

```synx
struct ChannelPair<T: Owned>
```

The two endpoints of a new channel. [channel](#channel) returns it. Take each
endpoint one time with [sender](#channelpairsender) and
[receiver](#channelpairreceiver).

### ChannelPair.sender

```synx
Sender<T> sender(&self)
```

Moves the first sender out of the pair and returns it.

Panics with `channel sender already taken` on a second call.

### ChannelPair.receiver

```synx
Receiver<T> receiver(&self)
```

Moves the receiver out of the pair and returns it.

Panics with `channel receiver already taken` on a second call.

### Sender

```synx
struct Sender<T: Owned>
```

The sending endpoint of a channel. Clone it to add more producers.

Dropping a sender lowers the sender count and wakes all threads that wait in
[recv](#receiverrecv). When the count reaches zero, `recv` returns `None`
after the queue is empty.

### Sender.clone

```synx
Sender<T> clone(&const self)
```

Returns a new sender for the same channel and adds one to the sender count.

Panics with `channel sender count overflow` if the count is already
9223372036854775807.

### Sender.send

```synx
Result<void, T> send(&const self, T value)
```

Adds *value* to the end of the queue and wakes one waiting receiver. Returns
`Ok(())`. The call does not wait for a receiver to take the value.

Returns `Err(value)` if the receiver was dropped. The value is given back to
the caller.

### Receiver

```synx
struct Receiver<T: Owned>
```

The receiving endpoint of a channel. `recv` takes `&const self`, so several
threads can share one receiver through an [Arc](sync.md#arc). Each value goes
to one caller.

Dropping the receiver closes the channel. Later `send` calls return `Err`.
The values that are still in the queue are dropped after the lock is
released, so their destructors can send to the same channel or drop a sender
of it.

### Receiver.recv

```synx
T? recv(&const self)
```

Removes the oldest value from the queue and returns `Some(value)`. If the
queue is empty, blocks until a value arrives or the last sender is dropped.

Returns `None` when the queue is empty and all senders were dropped. Values
sent before the last sender was dropped are returned first.

`recv` blocks the thread. In a task on a [worker pool](task.md#executor), it
blocks the worker thread.

## Functions

### channel

```synx
ChannelPair<T> channel<T: Owned>()
```

Creates an empty channel with one sender and one receiver, and returns them
in a [ChannelPair](#channelpair).

## Example

Three threads send values to one receiver:

```synx
import std.thread;
import std.channel;
import std.mem;

int main() {
    var pair = channel.channel<int>();
    var sender = pair.sender();
    var receiver = pair.receiver();
    var workers = Vec<thread.JoinHandle<void>>.new();
    for (id in 0..3) {
        var out = sender.clone();
        match thread.spawn(send once || {
            for (i in 1..11) { out.send(id * 100 + i); }
        }) {
            Ok(handle) => workers.push(handle),
            Err(_) => return 1,
        }
    }
    mem.drop(sender);
    int count = 0;
    int total = 0;
    loop {
        match receiver.recv() {
            Some(value) => { count += 1; total += value; },
            None => break,
        }
    }
    while (!workers.is_empty()) { workers.pop().unwrap().join(); }
    print("{count} values, total {total}");
    return 0;
}
```

The original sender is dropped before the loop. Without that drop, `recv`
waits forever after the workers finish.

Output:

```text
30 values, total 3165
```

Queued values survive the last sender, and a closed channel gives the value
back:

```synx
import std.channel;
import std.mem;

int main() {
    var pair = channel.channel<String>();
    var sender = pair.sender();
    var receiver = pair.receiver();
    sender.send(String.from("first"));
    sender.send(String.from("second"));
    mem.drop(sender);
    loop {
        match receiver.recv() {
            Some(text) => print("got {text}"),
            None => { print("all senders dropped"); break; },
        }
    }
    var other = channel.channel<String>();
    var tx = other.sender();
    mem.drop(other.receiver());
    match tx.send(String.from("lost")) {
        Ok(_) => print("sent"),
        Err(value) => print("receiver dropped, kept {value}"),
    }
    return 0;
}
```

Output:

```text
got first
got second
all senders dropped
receiver dropped, kept lost
```
