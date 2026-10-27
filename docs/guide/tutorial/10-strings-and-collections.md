# 10. Strings and collections

This chapter covers the two string types, the growable array `Vec<T>`, and
the hash table `HashMap<K, V>`. `String` and `Vec` are in the prelude.
`HashMap` and `sort` come from `std.collections`.

## Two string types

A string literal has the type `&const str`. It is a read-only reference to
text that lives for the whole program. A `String` owns its text on the heap.
It can grow, and it is destroyed with its owner.

`String.from` copies borrowed text into a new `String`. An interpolated
literal also makes a `String`:

```synx
main() {
    &const str title = "Life Is Strange";
    String copy = String.from(title);
    String line = "{copy} has {title.len()} bytes";
    print(line);
}
```

```text
Life Is Strange has 15 bytes
```

A `&const str` parameter accepts a literal and a `String`. The `String` is
borrowed, and the caller keeps it:

```synx
void announce(&const str song) {
    print("Now playing: {song}");
}

main() {
    var song = String.from("Karma Police");
    announce(song);
    announce("Airbag");
    print(song);
}
```

```text
Now playing: Karma Police
Now playing: Airbag
Karma Police
```

`as_str()` gives a `&const str` view of a `String`.

## Building a String

`String.new()` makes an empty string. `push_str` adds text and `push` adds
one `char`:

```synx
main() {
    var name = String.new();
    name.push_str("Dead");
    name.push_str("lift");
    name.push('!');
    print("{name} {name.len()} {name.is_empty()}");
    name.clear();
    print("{name.is_empty()}");
}
```

```text
Deadlift! 9 false
true
```

`len()` counts bytes of UTF-8. A character outside ASCII uses more than one
byte, so `len()` can be larger than the number of characters:

```synx
import std.text;

main() {
    var word = "café";
    print("{word.len()} bytes");
    print("{text.code_points(word).len()} characters");
}
```

```text
5 bytes
4 characters
```

## Comparing strings

`==`, `!=`, `<`, and the other comparison operators work on two `String`
values and on two `&const str` values. `<` compares bytes in order:

```synx
main() {
    var a = String.from("Bench");
    var b = String.from("Bench");
    var c = String.from("Squat");
    print("{a == b} {a != c} {a < c}");
    print("{"Bench" < "Bench Press"}");
}
```

```text
true true true
true
```

Both sides must have the same type. To compare a `String` with a literal,
use `as_str()`:

```synx
main() {
    var lift = String.from("Squat");
    if (lift == "Squat") {
        print("leg day");
    }
}
```

```text
example.snx:3:9: error: operator '==' needs both sides to have the same type, found 'String' and '&const str'
3 |     if (lift == "Squat") {
  |         ^^^^^^^^^^^^^^^
1 error
```

```synx
main() {
    var lift = String.from("Squat");
    if (lift.as_str() == "Squat") {
        print("leg day");
    }
}
```

```text
leg day
```

## String methods

`String` has these methods in the prelude: `starts_with`, `contains`, `trim`,
and `split`. `trim` and `split` return new owned values:

```synx
main() {
    var raw = String.from("  squat,bench,deadlift  ");
    var clean = raw.trim();
    print("[{clean}]");
    print("{clean.starts_with("squat")} {clean.contains("bench")}");
    var lifts = clean.split(",");
    print("{lifts.len()} lifts, the second is {lifts[1]}");
}
```

```text
[squat,bench,deadlift]
true true
3 lifts, the second is bench
```

A `&const str` has the methods `len()` and `parse_int()`. `parse_int()`
returns an `int?`, which is `None` when the text is not a whole number.

## The std.text module

`std.text` has functions that take `&const str` arguments. They also accept
a `String`:

```synx
import std.text;

main() {
    var plan = "5x5 at 225";
    print(text.replace(plan, "225", "235"));
    print(text.to_ascii_uppercase("bench"));
    print("{text.find(plan, "at") ?? 99}");
    print("{text.ends_with(plan, "225")}");
    var lines = text.lines("warmup\nwork\ncooldown");
    print(text.join(lines, " / "));
    print("{text.parse_long("-9000000000") ?? 0}");
}
```

```text
5x5 at 235
BENCH
4
true
warmup / work / cooldown
-9000000000
```

`find` returns the byte offset of the first match as a `usize?`. `slice`
copies a part of the text between two byte offsets. It returns `None` when
an offset is not at the start of a character:

```synx
import std.text;

main() {
    var word = "café";
    print(text.slice(word, 0, 3) ?? String.from("?"));
    print("{text.slice(word, 0, 4).is_none()}");
}
```

```text
caf
true
```

`std.utf8` converts between text and bytes. `encode` returns a `Vec<byte>`.
`decode` checks that the bytes are valid UTF-8 and returns a `String?`:

```synx
import std.utf8;

main() {
    var bytes = utf8.encode("hé");
    print("{bytes.len()} bytes");
    match utf8.decode(bytes) {
        Some(text) => print(text),
        None => print("invalid UTF-8"),
    }
}
```

```text
3 bytes
hé
```

The library reference lists every function in
[std.text](../../library/text.md) and [std.utf8](../../library/utf8.md).

## Vec

`Vec<T>` is a growable array. `push` adds an element at the end. `pop`
removes the last element and returns it as an `Option`:

```synx
main() {
    Vec<int> sets = Vec.new();
    sets.push(135);
    sets.push(185);
    sets.push(225);
    print("{sets.len()} sets, first {sets[0]}");
    var last = sets.pop();
    print("removed {last ?? 0}, {sets.len()} left");
}
```

```text
3 sets, first 135
removed 225, 2 left
```

When the declaration has no type, write the element type on `Vec`:

```synx
main() {
    var names = Vec<String>.new();
    names.push(String.from("Smoke on the Water"));
    print("{names.len()}");
}
```

```text
1
```

## Indexing and get

`v[i]` reads or writes the element at index `i`. An index out of range
causes a panic:

```synx
main() {
    Vec<int> reps = Vec.new();
    reps.push(5);
    reps.push(5);
    reps[1] = 3;
    print("{reps[0]} {reps[1]}");
    print("{reps[2]}");
}
```

```text
5 3
Synx panic: index out of bounds
```

`get(i)` returns an `Option` with a reference to the element. It returns
`None` when the index is out of range. `get_mut(i)` gives a writable
reference:

```synx
main() {
    Vec<int> waypoints = Vec.new();
    waypoints.push(120);
    waypoints.push(340);
    match waypoints.get(5) {
        Some(distance) => print("{*distance} m"),
        None => print("no waypoint 5"),
    }
    match waypoints.get_mut(0) {
        Some(distance) => { *distance += 30; }
        None => {}
    }
    print("first waypoint: {waypoints[0]} m");
}
```

```text
no waypoint 5
first waypoint: 150 m
```

## Iterating

A `for` loop over `&const v` gives a `&const T` for each element. A loop
over `&v` gives a `&T`, which can change the element:

```synx
main() {
    Vec<int> weights = Vec.new();
    weights.push(135);
    weights.push(185);
    weights.push(225);
    for (w in &weights) {
        *w += 10;
    }
    int total = 0;
    for (w in &const weights) {
        total += *w;
    }
    print("total {total}");
}
```

```text
total 575
```

A loop over the Vec itself, `for (w in weights)`, consumes it. Each iteration
owns one element. Unread elements are dropped if the loop exits early. Use a
reference when the vector must remain available after the loop.

## Sorting

`swap(i, j)` exchanges two elements. `sort` from `std.collections` puts the
elements in ascending order. The element type must implement `Ord`.
[Chapter 11](11-traits-and-generics.md) shows how to implement `Ord` for your
own types.

```synx
import std.collections.sort;

main() {
    Vec<String> songs = Vec.new();
    songs.push(String.from("Yellow"));
    songs.push(String.from("Clocks"));
    songs.push(String.from("Fix You"));
    sort(&songs);
    for (song in &const songs) {
        print(song);
    }
    songs.swap(0, 2);
    print("first: {songs[0]}");
}
```

```text
Clocks
Fix You
Yellow
first: Yellow
```

## HashMap

`HashMap<K, V>` maps keys to values. The key type must implement `Hash` and
`Eq`. The integer types, `bool`, `char`, `String`, and `&const str` do.

`insert` takes ownership of the key and the value. When the key is already
in the map, `insert` replaces the value and returns the old one:

```synx
import std.collections.HashMap;

main() {
    var best = HashMap<String, int>.new();
    best.insert(String.from("squat"), 315);
    best.insert(String.from("bench"), 225);
    var old = best.insert(String.from("squat"), 325);
    print("old squat {old ?? 0}, {best.len()} lifts");
}
```

```text
old squat 315, 2 lifts
```

`get`, `get_mut`, `contains_key`, and `remove` take a reference to a key.
For a map with `String` keys, the key must be a `String`:

```synx
import std.collections.HashMap;

main() {
    var stock = HashMap<String, int>.new();
    stock.insert(String.from("medkit"), 3);
    stock.insert(String.from("ammo"), 120);

    var key = String.from("medkit");
    match stock.get(key) {
        Some(count) => print("medkits: {*count}"),
        None => print("no medkits"),
    }
    match stock.get_mut(key) {
        Some(count) => { *count -= 1; }
        None => {}
    }
    print("contains medkit: {stock.contains_key(key)}");
    var removed = stock.remove(key);
    print("removed {removed ?? 0}, {stock.len()} left");
    print("contains medkit: {stock.contains_key(key)}");
}
```

```text
medkits: 3
contains medkit: true
removed 2, 1 left
contains medkit: false
```

`HashMap` has no iteration. Keep a `Vec` of the keys when a program must
visit every entry. See [std.collections](../../library/collections.md) for
the full interface.

## Next

[Chapter 11](11-traits-and-generics.md) covers traits and generic code.
