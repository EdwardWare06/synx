# unsafe block and extern declaration

Allows operations whose memory or external contract the author must check.

## Syntax

```text
unsafe { body }
extern "C" { return_type name(parameters); }
unsafe impl Send for Type { }
```

## Explanation

An unsafe block permits raw pointer dereferences, external calls, and unsafe
intrinsics. Its author must ensure valid addresses, alignment, initialized
values, access permissions, and storage lifetimes. Ordinary type checking
and ownership rules still apply to safe values within the block.

Extern declarations name functions with the supported platform C boundary.
They do not supply a function body or a library dependency automatically.
The chosen linker must be able to resolve the symbol. The current built-in
linker's import interface is centered on the Synx runtime capsule; it is
not a general command-line replacement for every system-linker option.

An explicit Send, Sync, or Owned implementation requires `unsafe impl`.
The author must uphold the marker's transfer, shared-access, or no-borrowed-
storage contract even when raw fields hide those properties.

## Example

```synx
main() {
    int value = 8;
    var pointer = (&value) as *int;
    unsafe { *pointer = 9; }
    print("{value}");
}
```

Output: `9`.

## Notes

`drop_in_place<T>` destroys an initialized value at an address and does not
free its storage. `str_from_raw` borrows bytes under the caller's storage
and UTF-8 contract. Unsafe code can invalidate memory safety; safe callers
depend on the correctness of its exposed interface.
