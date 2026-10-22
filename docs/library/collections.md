# std.collections

```synx
import std.collections;
import std.collections.{HashMap, sort};
```

This module holds the `HashMap` type and the `sort` function. It also
re-exports `Vec` from the [prelude](prelude.md#vec) and `Hash` from
[std.hash](hash.md#hash).

## Types

### HashMap

```synx
struct HashMap<K: Hash + Eq, V>
```

An owning hash table that maps keys of type `K` to values of type `V`. The
fields are private. The table uses open addressing with linear probing.

Equal keys must have equal hashes. The hash and the equality of a key must
not change while the key is in the map. Keys and values do not need `Copy`.
The map owns them. Dropping the map drops all keys and values and frees the
storage. `HashMap` is not `Copy`.

Lookup methods take the key as `&const K`. A `K` value passed as the key is
borrowed automatically. For a map with `&const str` keys, pass a reference to
the `&const str`, as in `map.get(&const key)`.

There is no method to iterate over the entries.

`HashMap<K, V>` is `Send`, `Sync`, or `Owned` when both `K` and `V` are.

### HashMap.new

```synx
HashMap<K, V> new()
```

Returns an empty map. No memory is allocated.

### HashMap.len

```synx
usize len(&const self)
```

Returns the number of entries.

### HashMap.capacity

```synx
usize capacity(&const self)
```

Returns the number of allocated slots. The capacity is zero before the first
insertion. Insertion keeps at least 25% of the slots empty.

### HashMap.is_empty

```synx
bool is_empty(&const self)
```

Returns `true` if the map has no entries.

### HashMap.contains_key

```synx
bool contains_key(&const self, &const K key)
```

Returns `true` if the map has an entry for *key*.

### HashMap.get

```synx
(&const V)? get(&const self, &const K key)
```

Returns a shared reference to the value for *key*. Returns `None` if there is
no entry for *key*.

### HashMap.get_mut

```synx
(&V)? get_mut(&self, &const K key)
```

Returns a writable reference to the value for *key*. Returns `None` if there
is no entry for *key*. There is no writable access to keys.

### HashMap.insert

```synx
V? insert(&self, K key, V value)
```

Moves *key* and *value* into the map. Returns `None` if *key* is new.

If the map already has an entry for *key*, the method stores *value* and
returns `Some` with the previous value. The map keeps the stored key and
drops *key*.

The first insertion allocates 8 slots. Before a new key is added, the method
checks the slots that hold entries or removed entries. If they fill 75% of
the capacity, the table is rebuilt. The new capacity is the same if fewer
than half of the slots hold entries. Otherwise the capacity doubles. A
rebuild moves keys and values. It does not drop them.

Panics with:

- `HashMap capacity is too large` if the new storage size does not fit in
  `usize`.
- `HashMap key hash or equality changed during insertion` if the `Hash` or
  `Eq` implementation of the key gives different results during one call.

### HashMap.remove

```synx
V? remove(&self, &const K key)
```

Removes the entry for *key*. Returns `Some` with the value, or `None` if
there is no entry for *key*. The stored key is dropped. The caller owns the
returned value.

### HashMap.clear

```synx
void clear(&self)
```

Drops all keys and values. The allocation is kept.

## Functions

### sort

```synx
void sort<T: Ord>(&Vec<T> values)
```

Sorts *values* in ascending order. The order comes from `Ord.lt`. The
function uses heapsort. It does not allocate memory. It moves elements and
does not copy or drop them. The worst case is O(n log n) comparisons.

The sort is unstable. Equal elements can change order. `Ord.lt` must give a
consistent strict order.

## Example

Count words and print them in sorted order:

```synx
import std.collections.{HashMap, sort};

main() {
    var words = String.from("the cat and the dog and the bird").split(" ");
    var counts = HashMap<String, int>.new();
    var unique = Vec<String>.new();
    for (word in &const words) {
        match counts.get_mut(word) {
            Some(count) => { *count += 1; }
            None => {
                counts.insert(String.from(word.as_str()), 1);
                unique.push(String.from(word.as_str()));
            }
        }
    }
    sort(&unique);
    for (word in &const unique) {
        print("{word}: {counts.get(word) ?? &const 0}");
    }
    print("{counts.len()} distinct words");
}
```

The output is:

```text
and: 2
bird: 1
cat: 1
dog: 1
the: 3
5 distinct words
```

Sort a user type, and replace and remove map entries:

```synx
import std.collections.{HashMap, sort};

struct Job {
    int priority;
    String name;
}

impl Ord for Job {
    bool lt(&const self, &const Self other) { return self.priority < other.priority; }
}

main() {
    var jobs = Vec<Job>.new();
    jobs.push(Job { priority: 3, name: String.from("deploy") });
    jobs.push(Job { priority: 1, name: String.from("build") });
    jobs.push(Job { priority: 2, name: String.from("test") });
    sort(&jobs);
    for (job in &const jobs) { print("{job.priority} {job.name}"); }

    var ages = HashMap<&const str, int>.new();
    print("{ages.insert("ada", 36) ?? -1}");
    print("{ages.insert("ada", 37) ?? -1}");
    var key = "ada";
    print("{ages.remove(&const key) ?? -1} {ages.len()} {ages.capacity()}");
}
```

The output is:

```text
1 build
2 test
3 deploy
-1
36
37 0 8
```
