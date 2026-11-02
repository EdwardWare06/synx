# Debugging

Linux and Windows x86-64 debug builds include source lines and function names.
Linux uses DWARF 4. Windows uses CodeView and a PDB file beside the executable.

## Linux

Use GDB to set source breakpoints, step through statements, and view the call
stack. Keep the source files at their build paths, or use GDB's
`set substitute-path` command after moving them.

```sh
synx build main.snx -o app
gdb ./app
```

```text
(gdb) set breakpoint pending on
(gdb) break main.snx:4
(gdb) run
(gdb) next
(gdb) step
(gdb) bt
(gdb) continue
```

Replace `4` with a line that contains an executable statement. Blank lines,
comments, declarations without emitted code, and statements removed during
lowering have no distinct instruction address.

Standalone programs map their native code at startup. GDB learns the code
addresses through its standard JIT registration interface. A breakpoint set
before `run` can remain pending until startup registers the source map.
Registration also retains the map for a debugger that attaches to a running
process. This interface does not compile or interpret Synx code at runtime.

`--shared-runtime` and `--linker=system` builds contain normal ELF debug
sections. Their source breakpoints are available when GDB opens the executable.
`--keep-obj` also retains the relocatable object with its DWARF sections.

## Windows

```powershell
synx build main.snx -o app.exe
```

Keep `app.pdb` beside `app.exe`. Open the executable in Visual Studio with
File > Open > Project/Solution, select native debugging, and open the Synx source.
Set a breakpoint on an executable statement. Start debugging, then use Step Into,
Step Over, Step Out, and Call Stack. A Windows debug adapter that reads native
PDB symbols can use the same files. The Synx extension supplies language services;
it does not include a native debug adapter.

The built-in linker writes the PDB itself. Visual Studio and `link.exe` are not
required to build it. `--linker=system` requests CodeView/PDB linking through
`link.exe`. `--keep-obj` retains the COFF object with its CodeView sections.

Standalone debug builds place generated code in normal PE image sections. Windows
can locate source symbols before startup and after attaching to a running process.
The PDB is an optional debugging companion. The executable runs without it.
Keep the matching EXE and PDB together: their GUID and age must match.

Source records include SHA-256 checksums of the compiled source bytes. Keep the
source files at the recorded paths, or configure the debugger's source search paths.

## Build options

- A Linux or Windows debug build enables source information by default.
- `--release` omits source information by default.
- `-g` enables it, including with `--release`. The order of these two options
  does not change the result.
- `--no-debug` disables it. If both `-g` and `--no-debug` occur, the last one
  takes effect.

```sh
synx build main.snx --release -g -o app
synx build main.snx --no-debug -o app-small
```

Debug information does not change integer overflow rules or register allocation.
The run cache includes the debug setting in its key.

## Current limits

Linux and Windows describe parameters, local variables, primitive types,
pointers, references, arrays, tuples, and concrete struct and enum layouts.
Use `info locals`, `info args`, and `print value` in GDB. Windows native
debuggers can show the same values in their locals and watch windows.

Location records follow stack slots and allocated registers. A value can become
unavailable after its last use, a move, or a scope end. A value initialized on
only some control-flow paths can also be unavailable. The debugger can report
`<optimized out>` even when the build has integer overflow checks enabled.
`--no-regalloc` keeps scalar values in stack slots and can help with inspection.

Enums expose `tag` and `payload`. Read the payload member selected by the tag;
the other union members do not contain active values. Tuple fields use `_0`,
`_1`, and so on. A borrowed string or slice exposes `data` and `len`. A dynamic
trait reference exposes `data` and `vtable`. String and Vec show their stored
fields. Automatic collection and string displays remain future work.

Debugger expressions use C syntax. Debugger-initiated Synx function and method
calls are unsupported. Closure captures and suspended async-frame variables
are not available through these records.

macOS debug output remains separate work. `-g` currently reports an error on
macOS. Ordinary builds still work there.

The current line tables use absolute source paths. They can disclose build
directory names. Use `--no-debug` when those paths must be omitted.

The source map uses GDB's C expression mode with the producer name `Synx Axon`.
Synx has no assigned DWARF language code in this implementation.
