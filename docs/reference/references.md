# Reference expression and type

Borrows a value without taking ownership of it.

## Syntax

```text
&expression
&const expression
&T
&const T
```

## Explanation

`&T` permits exclusive read and write access. `&const T` permits shared
read access. At a conflicting use, the checker allows one exclusive loan
or shared loans. It also checks that the borrowed storage is still alive.
Loan liveness can end after the final dependent use, before block exit.

A shared reference is Copy. An exclusive reference moves when passed by
value unless the compiler creates a reborrow. An ordinary `&T` argument
must be written explicitly. Method receivers are borrowed automatically.

A `&const` parameter can borrow a compatible value implicitly. Coercions
include `String` to `&const str`, arrays or Vec to shared slices, and
concrete references to compatible borrowed trait objects. An exclusive
reference can be used through a shared parameter.

## Example

```synx
void increment(&int value) { *value += 1; }
int read(&const int value) { return *value; }

main() {
    int count = 4;
    increment(&count);
    print("{read(count)}");
}
```

Output: `5`.

## Notes

There is no explicit lifetime-parameter syntax. References stored in
aggregates, returned values, tasks, or closures retain their loans. Checked
direct functions infer argument dependencies. Indirect and external calls
use conservative rules. Scope exit also checks loans retained by destructors.
See [Ownership](ownership.md) and [unsafe](unsafe.md).
