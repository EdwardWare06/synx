# Prelude

```synx
import std.prelude.{String, Vec};
```

The prelude holds the core types, traits, and functions. Every module can use
them without an import. The module path is `std.prelude`, so an explicit
import as above is also valid. `std.io` re-exports `print`, `std.string`
re-exports `String`, `std.collections` re-exports `Vec`, and `std.task`
re-exports `Task`, `SendTask`, `Future`, `Poll`, and `Waker`.

[std.hash](hash.md) implements `Hash` for `String`, `str`, and the integer
types. No other standard module adds implementations to prelude types. Free
functions that work on text are in [std.text](text.md).

## Types

### Option

```synx
enum Option<T> {
    None,
    Some(T),
}
```

An optional value. The type `T?` is the same type as `Option<T>`. The variant
names `Some` and `None` are in scope without the `Option.` prefix.

The postfix `?` operator and the `??` operator work on `Option`. See
[Error values and panics](../reference/errors.md).

### Option.is_some

```synx
bool is_some(&const self)
```

Returns `true` if the value is `Some`.

### Option.is_none

```synx
bool is_none(&const self)
```

Returns `true` if the value is `None`.

### Option.unwrap

```synx
T unwrap(self)
```

Returns the contained value. The call consumes the option.

Panics with `called Option.unwrap on None` if the value is `None`.

### Option.expect

```synx
T expect(self, &const str message)
```

Returns the contained value. The call consumes the option.

Panics with *message* if the value is `None`.

### Option.unwrap_or

```synx
T unwrap_or(self, T fallback)
```

Returns the contained value, or *fallback* if the value is `None`. The call
consumes the option and *fallback*.

### Option.as_ref

```synx
Option<&const T> as_ref(&const self)
```

Returns `Some` with a shared reference to the contained value, or `None`. The
option keeps its value.

### Option.as_mut

```synx
Option<&T> as_mut(&self)
```

Returns `Some` with a writable reference to the contained value, or `None`.

### Option.take

```synx
Option<T> take(&self)
```

Moves the value out of the option and leaves `None` in its place. Returns the
previous value.

### Option.replace

```synx
Option<T> replace(&self, T value)
```

Stores `Some(value)` in the option. Returns the previous value. The previous
value is moved to the caller and is not dropped.

### Result

```synx
enum Result<T, E> {
    Ok(T),
    Err(E),
}
```

A success value of type `T` or an error value of type `E`. The variant names
`Ok` and `Err` are in scope without the `Result.` prefix.

The postfix `?` operator returns an `Err` from the current function. `Result`
has only the two methods below. Use `match` or `?` to get the contained value.

### Result.is_ok

```synx
bool is_ok(&const self)
```

Returns `true` if the value is `Ok`.

### Result.is_err

```synx
bool is_err(&const self)
```

Returns `true` if the value is `Err`.

### Range

```synx
struct Range<T> {
    T start;
    T end;
}
```

The expression `a..b` builds a `Range`. Both fields are public. A `for` loop
over a range starts at `start` and stops before `end`. Both ends are
required. The compiler rejects a range without a start or an end.

### RangeInclusive

```synx
struct RangeInclusive<T> {
    T start;
    T end;
}
```

The expression `a..=b` builds a `RangeInclusive`. A `for` loop over it starts
at `start`. The last value is `end`.

### String

```synx
struct String
```

An owned, growable UTF-8 string. The fields are private. An interpolated
literal such as `"total: {n}"` builds a `String`.

A `String` argument to a `&const str` parameter is borrowed automatically.
`==`, `!=`, `<`, `>`, `<=`, and `>=` compare two `String` values byte by
byte. A shorter string that is a prefix of a longer string compares as less.
Both sides must have the same type. To compare a `String` with a `str`, call
[as_str](#stringas_str) on the `String`.

`String` is not `Copy`. Assignment and argument passing move it. A `String`
frees its storage when it is dropped.

### String.new

```synx
String new()
```

Returns an empty string. No memory is allocated.

### String.from

```synx
String from(&const str s)
```

Returns a new string that holds a copy of *s*.

### String.len

```synx
usize len(&const self)
```

Returns the length in bytes.

### String.is_empty

```synx
bool is_empty(&const self)
```

Returns `true` if the length is zero.

### String.as_str

```synx
&const str as_str(&const self)
```

Returns a shared view of the contents. The view borrows the string.

### String.push_str

```synx
void push_str(&self, &const str s)
```

Appends a copy of *s*. When the string grows, the capacity starts at 16 bytes
and doubles until the text fits.

Panics with `String capacity is too large` if the new length does not fit in
`usize`.

### String.push

```synx
void push(&self, char c)
```

Appends the UTF-8 encoding of *c*. The encoding is one to four bytes.

Panics with `String capacity is too large` if the new length does not fit in
`usize`.

### String.starts_with

```synx
bool starts_with(&const self, &const str prefix)
```

Returns `true` if the string starts with the bytes of *prefix*. An empty
*prefix* gives `true`.

### String.contains

```synx
bool contains(&const self, &const str text)
```

Returns `true` if the bytes of *text* occur anywhere in the string. An empty
*text* gives `true`.

### String.trim

```synx
String trim(&const self)
```

Returns a new string without the ASCII whitespace at the start and the end.
ASCII whitespace is the space and the bytes 9 to 13: tab, line feed, vertical
tab, form feed, and carriage return. The original string does not change.

### String.split

```synx
Vec<String> split(&const self, &const str separator)
```

Splits the string at each occurrence of *separator*. Returns the parts as new
strings, in order. The search starts after the end of the previous match.
Empty parts are kept. The result always holds at least one element. An empty
string gives one empty part.

Panics with `split requires a nonempty separator` if *separator* is empty.

### String.clear

```synx
void clear(&self)
```

Sets the length to zero. The allocation is kept.

### Vec

```synx
struct Vec<T>
```

A growable array of `T`. The fields are private. `Vec` is not `Copy`.
Dropping a `Vec` drops its elements and frees its storage.

The index expression `v[i]` reads or writes element *i*. It panics with
`index out of bounds` if *i* is not less than the length.

A `for` loop over `&const v` gives a shared reference to each element. A
`for` loop over `&v` gives a writable reference to each element. The compiler
rejects a `for` loop over a `Vec` by value.

### Vec.new

```synx
Vec<T> new()
```

Returns an empty vector. No memory is allocated.

### Vec.len

```synx
usize len(&const self)
```

Returns the number of elements.

### Vec.is_empty

```synx
bool is_empty(&const self)
```

Returns `true` if the vector has no elements.

### Vec.capacity

```synx
usize capacity(&const self)
```

Returns the number of elements that the current allocation can hold.

### Vec.as_ptr

```synx
*const T as_ptr(&const self)
```

Returns a raw pointer to the first element. The pointer is valid while the
storage of the vector stays alive and unchanged. A push can move the storage.
Reading through the pointer requires an `unsafe` block.

### Vec.as_mut_ptr

```synx
*T as_mut_ptr(&self)
```

Returns a writable raw pointer to the first element. The rules of
[as_ptr](#vecas_ptr) apply.

### Vec.push

```synx
void push(&self, T value)
```

Moves *value* to the end of the vector. The first allocation holds 4
elements. After that, the capacity doubles.

Panics with `Vec capacity is too large` if the new size in bytes does not fit
in `usize`.

### Vec.pop

```synx
T? pop(&self)
```

Removes the last element and returns it. Returns `None` if the vector is
empty. The capacity does not change.

### Vec.get

```synx
(&const T)? get(&const self, usize index)
```

Returns a shared reference to the element at *index*. Returns `None` if
*index* is not less than the length.

### Vec.get_mut

```synx
(&T)? get_mut(&self, usize index)
```

Returns a writable reference to the element at *index*. Returns `None` if
*index* is not less than the length.

### Vec.swap

```synx
void swap(&self, usize left, usize right)
```

Exchanges the elements at *left* and *right*. The elements are moved. They
are not copied or dropped.

Panics with `Vec.swap index out of bounds` if either index is not less than
the length. This check also applies when *left* and *right* are equal.

### Vec.clear

```synx
void clear(&self)
```

Removes and drops all elements, from the last to the first. The allocation is
kept.

### Box

```synx
struct Box<T>
```

An owning heap allocation for one value of type `T`. When a `Box` moves, its
value stays at the same address. Dropping a `Box` drops the value and frees
the allocation.

### Box.new

```synx
Box<T> new(T value)
```

Allocates memory for one `T` and moves *value* into it.

### Box.as_ref

```synx
&const T as_ref(&const self)
```

Returns a shared reference to the value.

### Box.as_mut

```synx
&T as_mut(&self)
```

Returns a writable reference to the value.

### Box.as_ptr

```synx
*const T as_ptr(&const self)
```

Returns a raw pointer to the value.

### Box.as_mut_ptr

```synx
*T as_mut_ptr(&self)
```

Returns a writable raw pointer to the value.

### Box.into_inner

```synx
T into_inner(self)
```

Moves the value out of the box and frees the allocation. The call consumes
the box.

### Poll

```synx
enum Poll<T> { Pending, Ready(T) }
```

The result of one call to [Future.poll](#future). `Pending` means the future
is not complete. `Ready` holds the output. See [std.task](task.md).

### Waker

```synx
struct Waker
```

A reference-counted handle that tells an executor to poll a future again.
A handle and its clones share one state. The state holds a ready flag and an optional
wake callback. The state is freed when the last handle is dropped. `Waker`
is `Send`, `Sync`, and `Owned`. See [std.task](task.md) for usage.

### Waker.new

```synx
Waker new()
```

Returns a waker without a callback. The ready flag of a new waker is set.

### Waker.from

```synx
Waker from<W: Wake + Send + Sync + Owned>(W value)
```

Moves *value* to the heap and returns a waker that calls `value.wake()` on
each [wake](#wakerwake). The value is dropped when the last handle is
dropped.

### Waker.clone

```synx
Waker clone(&const self)
```

Returns a new handle to the same state.

Panics with `too many waker references` if the reference count overflows.

### Waker.wake

```synx
void wake(&const self)
```

Sets the ready flag and wakes threads in [wait](#wakerwait). Then calls the
callback, if there is one.

### Waker.take_ready

```synx
bool take_ready(&const self)
```

Returns the ready flag and clears it.

### Waker.wait

```synx
void wait(&const self)
```

Blocks the thread until the ready flag is set. The flag stays set.

Panics with `blocking join on async worker; await the handle` if the thread
is a worker of a `std.task` executor that was not created for blocking jobs.

### Waker.is_retained

```synx
bool is_retained(&const self)
```

Returns `true` if more than one handle refers to the state.

### Task

```synx
struct Task<T>
```

The future that an `async` function returns. Only the compiler makes `Task`
values. A `Task` implements `Future<T>`. Dropping a `Task` cancels it. See
[std.task](task.md).

Its `poll` panics with `async task is already being polled` if the task is
polled again before the current poll returns.

### SendTask

```synx
struct SendTask<T: Send + Owned>
```

A `Task` that can move to another thread. `SendTask` is `Send` and `Owned`.
It implements `Future<T>` and has the same poll and drop behavior as
[Task](#task). See [std.task](task.md).

## Functions

### print

```synx
void print(&const str text)
```

Writes *text* and a line feed to standard output. The write is done while the
standard output lock is held. Lines printed by different threads do not mix.

### panic

```synx
never panic(&const str message)
```

Writes `Synx panic: `, *message*, and a line feed to standard error. Then
ends the process with exit status 101. Destructors do not run.

`never` is the type of an expression that does not return. A `panic` call can
stand where a value of any type is expected.

## Methods on str

The compiler supplies these methods on the built-in `str` type.

### str.len

```synx
usize len(&const self)
```

Returns the length in bytes.

### str.as_ptr

```synx
*const byte as_ptr(&const self)
```

Returns a raw pointer to the first byte.

### str.parse_int

```synx
Option<int> parse_int(&const self)
```

Reads the text as a decimal `int`. The text can start with one `+` or `-`.
All other bytes must be ASCII digits. Leading zeros are accepted.

Returns `None` if the text is empty, has no digits, contains another byte
such as a space, or gives a value outside the range of `int`. For wider
types, see `parse_long` and `parse_ulong` in [std.text](text.md).

## Traits

### Copy

```synx
trait Copy {}
```

Types that are copied bit for bit instead of moved. The compiler decides
which types are `Copy`. The `Copy` types are the primitive types, raw
pointers, shared references `&const T`, and arrays and tuples of `Copy`
types. A struct or enum is `Copy` if all its fields are `Copy` and it has no
`Drop` implementation. `String`, `Vec`, writable references, and function
values are not `Copy`. In generic code, a type parameter is `Copy` only with
a `Copy` bound.

### Send

```synx
trait Send {}
```

Values whose ownership can cross a thread boundary. See
[Marker traits](#marker-traits).

### Sync

```synx
trait Sync {}
```

Values that allow shared references on several threads. See
[Marker traits](#marker-traits).

### Owned

```synx
trait Owned {}
```

Values that contain no borrowed storage, including captured values. See
[Marker traits](#marker-traits).

### Marker traits

The compiler derives `Send`, `Sync`, and `Owned` from the structure of a
type:

- Primitive types and `str` have all three.
- A struct, enum, tuple, or array has a marker if all its fields or elements
  have it.
- A raw pointer has none of the three. A type with a raw pointer field needs
  an explicit implementation.
- A reference is never `Owned`. `&const T` is `Send` if `T` is `Sync`. `&T`
  is `Send` if `T` is `Send`. A reference is `Sync` if `T` is `Sync`.
- A `send` function value is `Send` and `Owned`. Function values are never
  `Sync`.

An explicit implementation must be written as `unsafe impl`. The prelude has
these implementations: `String` has all three. `Vec<T>` and `Box<T>` have
each marker that `T` has. `Waker` has all three. `SendTask<T>` is `Send` and
`Owned`.

### Drop

```synx
trait Drop {
    void drop(&self);
}
```

Types with a destructor. The compiler calls `drop` when the owner of a value
goes out of scope. A type with a `Drop` implementation is not `Copy`. To drop
a value early, see [std.mem.drop](mem.md#drop).

### Eq

```synx
trait Eq {
    bool eq(&const self, &const Self other);
}
```

Equality. The `==` operator calls `eq` on a user type. The `!=` operator
calls `eq` and negates the result.

The integer types, the floating-point types, `char`, `bool`, `str`, and
`String` implement `Eq` without an implementation in source. A reference
implements `Eq` if its target type does. The comparison operators look
through references on both sides. Both sides must have the same type.

### Ord

```synx
trait Ord {
    bool lt(&const self, &const Self other);
}
```

Ordering. On a user type, `a < b` calls `a.lt(b)` and `a > b` calls
`b.lt(a)`. `a >= b` is the negation of `a.lt(b)`, and `a <= b` is the
negation of `b.lt(a)`. The operands are evaluated from left to right.

The integer types, the floating-point types, `char`, `str`, and `String`
implement `Ord` without an implementation in source. `bool` does not
implement `Ord`. `str` and `String` compare bytes in order.

### Display

```synx
trait Display {
    String to_string(&const self);
}
```

Conversion to text. Each `{expr}` in an interpolated string calls
`to_string` on a user type. Every value in an interpolated string must
implement `Display`.

All primitive types except `void` implement `Display`, and so do `str` and
`String`. A reference implements `Display` if its target type does. A
floating-point value with no fractional part and a magnitude below 10^15
prints with one decimal place, such as `2.0`. Other floating-point values
print in the shortest form that reads back as the same value.

### Add, Sub, Mul, Div, Rem

```synx
trait Add { Self add(self, Self other); }
trait Sub { Self sub(self, Self other); }
trait Mul { Self mul(self, Self other); }
trait Div { Self div(self, Self other); }
trait Rem { Self rem(self, Self other); }
```

Arithmetic operators. On a user type, `a + b` calls `a.add(b)`, and `-`, `*`,
`/`, and `%` call `sub`, `mul`, `div`, and `rem`. The call consumes both
operands. The integer and floating-point types implement these traits
without an implementation in source.

### Neg

```synx
trait Neg { Self neg(self); }
```

The unary `-` operator. On a user type, `-a` calls `a.neg()`. The signed
integer types and the floating-point types implement `Neg`. The compiler
rejects `-` on an unsigned type.

### Not

```synx
trait Not { Self not(self); }
```

Logical and bitwise complement. The compiler treats `bool` and the integer
types as implementing `Not`. The `!` operator accepts only `bool` operands.
It does not call `not` on a user type.

### Wake

```synx
trait Wake { void wake(&const self); }
```

A wake callback. Several threads can call `wake` at the same time through a
shared reference. Pass a `Wake` value to [Waker.from](#wakerfrom). See
[std.task](task.md).

### Future

```synx
trait Future<T> { Poll<T> poll(&self, Waker wake); }
```

A computation that completes later with a value of type `T`. `poll` returns
`Poll.Ready` with the output, or `Poll.Pending`. A pending future calls
*wake* when it can make progress. `Task<T>` and `SendTask<T>` implement
`Future<T>`. See [std.task](task.md).

## Intrinsics for unsafe code

The compiler supplies these functions. The prelude uses them to build
`String`, `Vec`, and `Box`.

### str_from_raw

```synx
&const str str_from_raw(*const byte data, usize length)
```

Returns a `str` view of *length* bytes at *data*. The bytes are not copied or
checked. The caller must keep the bytes alive while the view is in use, and
the bytes must be valid UTF-8. The compiler does not require an `unsafe`
block for this call.

### size_of

```synx
usize size_of<T>()
```

Returns the size of `T` in bytes.

### drop_in_place

```synx
void drop_in_place<T>(*T value)
```

Runs the destructor of the value at *value*. The memory is not freed. The
call must be inside an `unsafe` block. The compiler reports error SNX0415
otherwise.

## Example

Parse comma-separated numbers and report the first bad field:

```synx
Result<int, String> total(&const str line) {
    int sum = 0;
    var fields = String.from(line).split(",");
    for (field in &const fields) {
        var text = field.trim();
        match text.as_str().parse_int() {
            Some(n) => { sum += n; }
            None => return Err("bad field '{text}'"),
        }
    }
    return Ok(sum);
}

main() {
    for (line in ["1, 2, 3", "10,x"]) {
        match total(line) {
            Ok(sum) => print("sum {sum}"),
            Err(message) => print("error: {message}"),
        }
    }
}
```

The output is:

```text
sum 6
error: bad field 'x'
```

Use `Display`, `Vec`, `String`, and `Box` together:

```synx
struct Point { int x; int y; }

impl Display for Point {
    String to_string(&const self) { return "({self.x}, {self.y})"; }
}

main() {
    var points = Vec<Point>.new();
    points.push(Point { x: 1, y: 2 });
    points.push(Point { x: 3, y: 4 });
    match points.get(1) {
        Some(p) => print("second: {p}"),
        None => print("empty"),
    }
    Point? last = points.pop();
    print("last: {last.unwrap()}, left: {points.len()}");

    var name = String.new();
    name.push_str("Synx");
    name.push('!');
    print("{name} has {name.len()} bytes");

    var boxed = Box.new(Point { x: 5, y: 6 });
    boxed.as_mut().x = 50;
    Point inner = boxed.into_inner();
    print("{inner}");
}
```

The output is:

```text
second: (3, 4)
last: (3, 4), left: 1
Synx! has 5 bytes
(50, 6)
```
