# std.hash

```synx
import std.hash;
import std.hash.{Hash, hash_str, combine};
```

This module holds the `Hash` trait, its implementations for the built-in
types, and two helper functions. [HashMap](collections.md#hashmap) uses
`Hash` to place keys. The hashes are deterministic and have no key. They are
not cryptographic. Do not use hash values as a stored data format.

## Traits

### Hash

```synx
trait Hash {
    ulong hash(&const self);
}
```

Returns a 64-bit hash of the value. Equal values must return equal hashes. A
type that implements `Hash` for use as a map key must also implement `Eq`.

This module implements `Hash` for these types:

- `byte`, `short`, `int`, `long`, `uint`, `ulong`, and `usize`. The value is
  converted to `ulong`. Its eight bytes are hashed from the low byte to the
  high byte.
- `bool`. `true` hashes as the integer 1 and `false` as the integer 0.
- `char`. The code point is hashed as an integer.
- `String`, `str`, and `&const str`. The UTF-8 bytes are hashed with
  [hash_str](#hash_str). A `String` and a `str` with the same text have the
  same hash.
- `&const T` for each `T` that implements `Hash`. The reference hashes as
  the value it refers to.

The floating-point types and writable references `&T` have no `Hash`
implementation.

## Functions

### hash_str

```synx
ulong hash_str(&const str text)
```

Returns the hash of the UTF-8 bytes of *text*. The function uses 64-bit
FNV-1a. Then it combines the high 32 bits into the low 32 bits with XOR.

### combine

```synx
ulong combine(ulong seed, ulong field_hash)
```

Mixes *field_hash* into *seed* and returns the result. The function rotates
*seed* left by 13 bits, applies XOR with *field_hash*, and multiplies by the
64-bit FNV prime. Then it combines the high 32 bits into the low 32 bits with
XOR.

Use it to hash a type with more than one field. Pass the field hashes in a
fixed order. The result depends on the order of the arguments.

## Example

Hash a key type with two fields and use it in a `HashMap`:

```synx
import std.hash.{Hash, hash_str, combine};
import std.collections.HashMap;

struct Cell {
    int row;
    int column;
}

impl Hash for Cell {
    ulong hash(&const self) { return combine(self.row.hash(), self.column.hash()); }
}

impl Eq for Cell {
    bool eq(&const self, &const Self other) {
        return self.row == other.row && self.column == other.column;
    }
}

main() {
    print("{hash_str("abc") == String.from("abc").hash()}");
    print("{combine(1, 2) == combine(2, 1)}");

    var grid = HashMap<Cell, String>.new();
    grid.insert(Cell { row: 0, column: 1 }, String.from("B1"));
    match grid.get(Cell { row: 0, column: 1 }) {
        Some(name) => print("found {name}"),
        None => print("missing"),
    }
}
```

The output is:

```text
true
false
found B1
```
