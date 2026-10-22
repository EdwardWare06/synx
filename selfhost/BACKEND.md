# Backend byte layer

`x64.snx` ports `compiler/src/x64.cpp`. `objfile.snx` ports
`compiler/src/objfile.cpp`. Axon instruction selection and register allocation
remain separate passes.

`target.snx` ports `TargetOs`, `Target`, and `Axon::symbol_for`. The driver
selects the OS explicitly. Target format, C ABI selection, names, and default
arithmetic/allocation settings match stage 0. Symbol names preserve ASCII
letters and digits, replace other bytes with `_`, and append an eight-digit
32-bit FNV-1a suffix. Multiplication uses a wider temporary before truncation,
so hashing works with checked arithmetic enabled.

## Encoder

`Assembler.new()` returns an empty function buffer. The assembler owns `code`,
`relocs`, and label fixups. Register parameters use hardware codes: general
registers 0–15, XMM registers 0–7, ALU operations 0–7, conditions 0–15. Public
constants provide the names from the C++ encoder with `ALU_` and `COND_` prefixes.

`new_label()` returns the next zero-based label ID. `bind(label)` records the
current code offset. `finish()` patches each pending rel32 field, then clears
its fixups. Bind each referenced label before finishing. Later instructions may
reference a bound label; call `finish()` again to apply those fixups. Calling
`finish()` without new fixups leaves the bytes unchanged.

`is_bound(label)` reports whether the label has an offset. `label_pos(label)`
returns the offset as `usize`; an unbound label returns the conversion of -1.
Label IDs and code offsets must fit signed 32-bit values, as in stage 0.

`lea_rip(dst, symbol, addend)` records a `Pc32` relocation with `addend - 4`.
The subtraction must fit `long`. `call_sym(symbol)` records `Call32` with -4.
Both methods own a copy of the symbol name. Memory operands always use disp32.

## Object writers

`write_object(&const ObjectFile obj, ObjectFormat format) -> Vec<byte>` returns
the complete file bytes. Formats are `Elf`, `Coff`, and `MachO`. The input owns
text, read-only data, symbol names, and relocation names. Serialization leaves
the input unchanged. Each relocation selects text or read-only data through
`is_text`; its offset is relative to that section.

`Call32` and `Pc32` require four bytes at the relocation offset. `Abs64` requires
eight bytes. The complete field must fit its section. Counts, offsets, and names
must fit the selected object format. Checked bounds failures panic. These
modules consume compiler-generated records rather than untrusted object files.

ELF stores explicit RELA addends. COFF and Mach-O copy each section and patch
implicit addends into that copy. Absolute relocations use the input addend;
relative relocations use `addend + 4`, which must fit `long`. Negative addends
are emitted as two's-complement bits. Mach-O prefixes symbol names with `_`.
Symbol iteration and duplicate-name index replacement match the C++ writer.

## Parity

`tests/selfhost_backend_test.cpp` generates input records and expected bytes
with the C++ encoder and object writers. `backend_probe.snx` reads the same
inputs and writes its results. `selfhost_backend_0..3` select default, stack,
release, and stack/release compiler modes.

All integers in the private probe protocol are little-endian. A blob is an
eight-byte length followed by that many bytes. Names are UTF-8 blobs, including
embedded zero bytes. Reader errors panic. The probe rejects trailing input.

Input starts with an eight-byte program count. Each program has an eight-byte
label count, an eight-byte operation count, then operation records. Each record
contains opcode, `a`, `b`, `c`, and `d` as one byte each, the immediate as eight
bytes, and a name blob. Opcode mappings are defined in the probe dispatch and
the test's `Opcode` enum. The harness checks code bytes, relocation offsets,
names, kinds, addends, and label state before and after execution.

An eight-byte object count follows the programs. Each object contains text and
read-only data blobs, an eight-byte symbol count, symbol records, an eight-byte
relocation count, and relocation records. A symbol is a name blob, eight-byte
offset, and one-byte `is_text`, `global`, and `defined` flags. A relocation is
an eight-byte offset, name blob, one-byte kind, eight-byte addend, and one-byte
`is_text` flag. Relocation kinds are Call32=0, Pc32=1, Abs64=2.

Target settings for each OS and flag combination follow the object reports.
Input then supplies an eight-byte symbol-name count and name blobs; output
contains that count and the corresponding mangled names.

The output records all three complete object files and both unchanged input
sections. Fixtures cover register pairs, immediate boundaries, displacement
boundaries, forward and backward labels, repeated fixups, every relocation kind
in both sections, symbol visibility and duplicates, UTF-8 and zero bytes in
names, empty sections, alignment boundaries, and deterministic generated objects.

Set `SYNX_KEEP_BACKEND=1` when running the test executable to retain the probe,
input corpus, and report after a successful comparison. The test prints their
directory. On Linux, run that probe with `LD_PRELOAD` set to the host `libasan.so`
and `ASAN_OPTIONS=detect_leaks=1` for an allocation check over the same corpus.
Probe arguments are `input.bin actual.bin` in the retained directory.
