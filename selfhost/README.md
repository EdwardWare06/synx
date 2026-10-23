# Synx compiler

The Synx compiler is written in Synx. These modules implement the source,
lexer, parser, declaration, module-loading, signature-resolution,
body-checking, Dendrite lowering, borrow-checking, and async frame conversion
stages, plus type layout, register allocation, the x86-64 encoder, the object
writers, instruction selection, the built-in linker (`link/`), and the driver
(`main.snx`, `driver*.snx`, `build.snx`).

The first compiler was written in C++ (stage 0). Each module here was ported
from it and compared byte for byte against it before release 0.1.0. The C++
compiler was retired after 0.1.0. A source build now starts from the
previous Synx release (the seed, `scripts/seed.json`); see "Bootstrap" below.
Sections that name a C++ file or stage 0 describe the behavior that the port
preserves.

## Modules

- `bytes.snx`: bounded byte access, copying, and comparison.
- `source.snx`: owned source bytes, file IDs, spans, line tables.
- `diagnostics.snx`: severity, records, error counts, byte rendering.
- `token.snx`: token kinds, names, classification, decoded values.
- `lexer.snx`: comments, identifiers, literals, punctuation, interpolation.
- `lexer_probe.snx`: differential test driver for the lexer.
- `ast.snx`: syntax tree arenas. Nodes refer to each other by `uint` index;
  `NONE` marks an absent child. One struct per node family with a kind field.
- `parser.snx`: recursive descent parser, a function-by-function port of
  `compiler/src/parser.cpp`, including speculation, recovery, and span rules.
- `ast_dump.snx`: strict dump, byte-identical to `compiler/src/ast_strict.cpp`.
- `parser_probe.snx`: differential test driver for the parser.
- `ids.snx`: typed arena handles (`TypeId`, `DefId`, `ModuleId`) and `NONE`.
- `types.snx`: type interner, predicates, formatting, substitution.
- `types_probe.snx`: differential test driver for the type interner.
- `defs.snx`: definition, module, and symbol-table arenas.
- `declare.snx`: builtins and the declaration pass.
- `modules.snx`: module discovery, import binding, and the stage driver.
- `resolve.snx`: signature resolution: type expressions, bounds, fields,
  variant payloads, impl headers, parameters, returns, trait method sets.
- `check_data.snx`: checker result tables, node keys, name references,
  coercions, locals, capture modes.
- `check.snx`: body checker: inference, coercions, names, methods, traits
  and obligations, patterns and exhaustiveness, closures, async, and the
  whole-program validation passes.
- `ir.snx`: Dendrite IR: places, operands, rvalues, statements,
  terminators, functions, vtables, and the module drop tables.
- `lower.snx`: lowering from checked syntax to Dendrite, one concrete
  instantiation at a time.
- `borrow.snx`: move analysis, drop elaboration, liveness, loans, escape
  and return checks, and the async liveness and loan tables.
- `async_frame.snx`: async frame conversion: heap frame layout, the poll
  state machine, cancellation drop order, and the send-task check.
- `layout.snx`: x86-64 size, alignment, field offsets, enum payloads.
- `regalloc.snx`: block liveness and linear scan register allocation for
  scalar locals.
- `axon.snx`: instruction selection, frames, drop helpers, the thread and
  entry bridges, and the object file for one target.
- `module_dump.snx`: declaration, signature, checker, and Dendrite dumps,
  byte-identical to `compiler/src/module_dump.cpp`.
- `modules_probe.snx`: differential test driver for declarations and signatures.

## Source contract

`SourceFile.new(String path, Vec<byte> data, uint id)` takes ownership.
It accepts malformed UTF-8 and embedded NUL bytes. Source bytes are immutable.
Sources above 4294967291 bytes panic; spans use 32-bit byte offsets.

`line_col(uint offset)` clamps offsets to EOF. Lines and byte columns are
one-based. `line_text(uint line)` returns owned bytes without trailing CR/LF;
invalid line numbers return empty.

`Span` is a half-open range in one source. Require `start <= end`.
`Span.merge` retains the first span's file ID.

## Lexer contract

`Vec<Token> tokenize(&const SourceFile file, &DiagnosticSink sink)` appends
diagnostics and returns owned tokens ending in `Eof`. Borrows end on return.

Token text is a byte vector. It retains identifier bytes, decoded string/char
bytes, doc-comment bodies, or numeric source text. Integer values use `ulong`.
Both floating-point token kinds store `double`; narrowing occurs later.

Recovery, keyword recognition, nested comments, interpolation frames, and
UTF-8 decoding match `compiler/src/lexer.cpp`. The bootstrap decoder accepts
some noncanonical UTF-8 sequences; the port preserves that behavior.

## Diagnostics

`DiagnosticSink` owns records. Rendering returns bytes, including malformed
source bytes. Sources resolve by file ID; the last registered match wins.
Missing IDs use the fallback source. Rendering retains no source references.

## Decimal conversion

`synx_rt_parse_double(data, length, out)` copies ASCII decimal input into a
NUL-terminated buffer and calls C `strtod`. It returns `0`, `EINVAL`, or
`ERANGE` and always writes `out`. Input grammar:

```text
[0-9]+ (.[0-9]+)? ([eE][+-]?[0-9]+)?
```

The decimal point is a literal `.`. Separators and suffixes are removed by
the lexer. Conversion uses the process C locale. Underflow errors follow
the host C library. Before 0.1.0, results matched stage 0 on the same host.
This native conversion remains part of the C runtime boundary.

## Lexer probe

`lexer_probe.snx` writes the report below for a manifest of sources. CI runs
it under the leak checker. The fields it reports:

- Token kind names, keyword flags, primitive-type flags.
- Token kind, file ID, span, decoded bytes, integer value, double bits.
- Diagnostic severity, file ID, span, message, code, error count, rendered bytes.
- Every source byte position, EOF clamping, line count, line text, span merging.
- Source selection, fallback, warnings, notes, empty and multiline spans.

The probe takes `<manifest> <report>`. The manifest contains one UTF-8 path
per line. Empty lines are ignored. File IDs start at 7.

Report integers are unsigned little-endian. A blob is a u64 byte length
followed by bytes. The report begins with a u32 token-kind count, then each
kind's name blob and two u8 flags. Each source appends:

1. u64 token count; records: u32 kind, three u32 span fields
   (`start`, `end`, `file`), text blob, u64 integer, u64 double bits.
2. u64 error count, u64 diagnostic count; records: u32 severity, span,
   message blob, code blob; rendered-diagnostics blob.
3. u32 line count; `(line, column)` u32 pairs for offsets 0 through byte length
   plus one; line-text blobs for lines 0 through line count plus one.
4. Merged span; source-selection diagnostic blob.


## Parser contract

`Parser.new(file, tokens, sink)` takes ownership of the tokens and the sink.
Lex into the sink first, then move it in. After `parse_module()` (or one of
the entry points `parse_expression`, `parse_type_only`, `parse_pattern_only`,
`parse_statement_only`), read `parser.ast` and `parser.sink`. `position()` is
the index of the next unread token. Parse functions return node indexes.

The tree is compared through `ast_dump`, which prints every field and span.
Both parsers must agree on the tree, the diagnostics, and the token position
for the repository sources, a generated operator matrix, error-recovery cases,
token-level mutations, and nesting 200 deep. Run
`ctest -R selfhost_parser` (one entry per compiler build mode).

## Semantic types

`TypeContext.new()` creates the twelve primitive types, `str`, `never`, and
`error`. Constructors return `TypeId` handles. Equal type IDs denote equal
types within one context. Inference variables always get fresh IDs.

`TypeContext.param(owner, index, name)` interns by definition ID and parameter
index. The first name is retained. Nominal types intern by definition ID and
type arguments; their qualified name is copied at declaration.

`Subst.bind(from, to)` replaces a previous binding for the same parameter.
`Subst.apply(ctx, id)` recursively replaces parameters and interns the result.
An empty substitution preserves IDs. `TypeId.none()` propagates unchanged.

`types_probe.snx` takes one report path. Reports contain sample count (u64),
inference count (u32), then a name blob and thirteen u8 predicate flags per
sample, followed by the row-major u8 identity matrix. CI runs it under the
leak checker.

## Declaration and module loading

`ModuleLoader.new(user, prelude, std_root)` takes ownership of both sources.
It assigns file IDs 0 and 1. Imported file IDs start at 2. Source and syntax
arenas remain owned by the loader. `run()` is called once; it returns false
when parsing, filesystem access, declaration, or import binding fails.
Diagnostics and partial semantic state remain available through `resolver`.

`prelude.snx` is the prelude. `prelude_source.snx` embeds its bytes in the
compiler; regenerate it with `embed_prelude.snx` after a prelude change. The
`cli` test checks that the two agree. `run()` stops before signature
resolution.

`DefId`, `ModuleId`, and `TypeId` own no references. Definition IDs contain an
arena kind and index. `Definition.ast_file` and `ast_index` identify the source
syntax node. Function records index the function arena; other records index
module items. `NONE` denotes an absent record. Records must stay in creation
order. Symbol tables use unsigned byte order and binary search. Import
namespace overlays belong to one importer and fall back to their base module.

`standard_library(fallback_root)` checks `SYNX_STD_PATH`, then four executable
ancestor folders, then the configured source fallback. It returns `IoError`
on host failure. Empty environment values disable the library on POSIX;
Windows ignores an empty value. `std.prelude` remains available in either case.
Filesystem operation failures report `SNX0303` at the first entry item span.

`synx modules <file>` prints the declaration dump. IDs use `kind#index`.
Module and definition records use creation order; symbol tables use map order.
Records include spans, visibility, namespace owners/bases, prelude links,
function owners, generic parameters, declaration worklists, builtin str
methods, and diagnostic severity/code/span/message. Non-ASCII bytes, quotes,
and backslashes use `\xHH`. No pointer address appears in the report.

`modules_probe <prelude> <std-fallback> <manifest> <report>` reads one UTF-8 entry
path per manifest line. For each entry it writes two length-prefixed blobs:
the declaration dump and rendered diagnostics. CI runs it under the leak
checker for successful imports and failures.

## Signature resolution

`ModuleLoader.resolve_signatures()` runs after `run()` returns true. Call it
at most once. It returns false when resolution reports an error; partial
state and diagnostics remain readable through `resolver`. It ports
`Resolver::resolve_signatures` with the same order: struct fields, enum
payloads, trait bounds, impl headers and method `Self`, function signatures,
impl/trait method sets, const types. Each definition resolves with its own
module active; the previous active module is restored.

`resolve.signatures(&Resolver, &const Vec<ast.Ast> trees)` reads syntax by
`Definition.ast_file` and `ast_index` and stores no tree references. Function
generic parameters are declared here, as in C++, so duplicate function type
parameters report during this stage. `Scope` lists generic owners innermost
first; the innermost non-none `Self` applies. Parameter types intern by owner
`DefId` and index.

`synx modules <file> --signatures` prints the dump for this stage after
`Compilation::run_signatures()`. The dump adds struct fields, variant tuple
types and fields, generic bounds `(trait type...)`, and impl trait references.
Each type prints its `to_string` text followed by `{owner:index ...}` for the
type parameters it contains, in inner/elements/return pre-order.

`modules_probe <prelude> <std-fallback> <manifest> <report> signatures`
resolves signatures for each entry whose declarations succeed. The combined
comparison runs the module corpus plus signature fixtures: every type form, every signature diagnostic, qualified
and private type paths through namespace views and re-exports, shadowed and
bounded generics, and token mutations of a signature-heavy entry.

## Body checking

`Checker.new(resolver)` takes ownership of a resolver whose signatures
resolved without errors. `check_program(&trees)` checks every constant and
non-builtin function, then runs the layout, Wake/Future signature, literal
range, and validation passes, matching `Checker::check_program`. Results are
in `checker.result`; diagnostics are in `checker.r.sink`.

Syntax nodes are keyed by `node_key(file, index)`: the file ID in the high 32
bits and the arena index in the low 32 bits. Field-initializer shorthand uses
`field_key(file, expression, position)`. Tables iterate in first-insertion
order; replacing a value keeps its position, as `InsertionMap` does in C++.
Function and constant locals are keyed by arena index.

Function contexts are frames on a stack. A closure frame names its enclosing
frame and shares the locals of the root frame. The checker keeps no
references into syntax trees; each method takes the tree vector.

`modules_probe ... check` checks bodies when signatures succeed and appends
the checker tables to the declaration dump. `synx modules <file> --check`
prints the dump for this stage.

## Dendrite lowering

`Lowerer.new(checker)` takes ownership of a Checker whose bodies checked
without errors. `lower_program(&trees, require_main)` lowers every
non-generic user function, then drains the worklist of instantiations,
closures, async bodies, and drop implementations. Returns false after an
SNX0601 or SNX0701 error; partial IR stays in `lowerer.module`.

Functions and vtables are indexes into `Module.functions` and
`Module.vtables`, in creation order. A function's `closure` is the closure
expression's node key. Sorted maps (`async_suspends`, `async_drop_flags`) are
vectors kept in key order. The drop tables keep insertion order for lookup;
the dump prints them by type text, as C++ does for its pointer-keyed maps.

`modules_probe ... lower` lowers when bodies check and appends the strict IR.
`synx modules <file> --lower` prints the dump for this stage.

## Borrow checking

`BorrowChecker.new().check_module(&checker, &module)` checks every function
in `Module.functions` order and returns false after an ownership error. It
moves each function out of the module while it checks it, then puts it back.
Drop elaboration rewrites the function in place: drop flags, drops of the
parts a partial move left behind, and drops at the start of a call's
continuation block. The async tables (`async_live`, `async_borrows`) are
merged per suspend point in key order.

`modules_probe ... borrow` checks ownership when lowering succeeds and appends
the strict IR after the pass. `synx modules <file> --borrow` prints the dump for this stage.

## Async frame conversion

`async_frame.convert(&checker, &trees, &module)` runs after ownership checks.
For each async factory it checks a send task first: every value live across a
suspension must be Send, and a reference counts as Send for the frame when
its referent is live too (SNX0420). It then orders cancellation drops for
each state by the may-borrow graph, largest local first when the order is
free (SNX0507 on a cycle), and rewrites the factory, poll, and cancel
functions. The frame is a tuple: the state word, then every poll local except
the waker. State 0 is the start, suspend points take 1, 2, ... in block
order, and the maximum `ulong` marks completion. The module name is
`async_frame` because `async` is a keyword.

Two C++ side effects are reproduced: `async_borrows[block]` inserts an empty
set for each state block and for each live block of a send task, and after
SNX0507 the poll function keeps empty locals and blocks.

`synx modules <file> --async` prints the dump for this stage.

## Layouts and register allocation

`LayoutTable.new()` caches layouts by `TypeId`; `of(&checker, t)` returns a
copy. `allocate_registers(&checker, &function, &table)` returns one register
number per local (-1 keeps the stack slot) and the callee-saved registers in
first-use order. Register numbers are x86-64 encodings: R8 and R9 for
intervals that do not cross a call, then RBX and R12 to R15. Locals are
pinned to the stack when their address is taken, when an rvalue writes
through an address, or for float casts, drops, and projections.

`module_dump.dump_machine(&checker, &module)` prints layouts and register
assignments. `synx modules <file> --machine` prints the dump for this stage.

## Instruction selection

`Axon.new(target).generate(&checker, &trees, &module)` writes every lowered
function with a body, each drop helper, the thread bridge, and the entry stub
into `axon.obj`; `object_bytes` serializes it with `write_object`. Before
generation Axon copies the symbol names, drop types, and drop
implementations out of the module. `Gen` owns the Axon state and one
function while it generates that function: each function moves out of the
module and back with `mem.replace`. Its methods take the checker as a
parameter instead of a stored reference, because a stored reference makes the
borrow checker assume that reference arguments escape into it.

`module_dump.dump_objects` prints one line per target: name, result, size,
and FNV-1a 32 hash of the object file.

## Combined probe

`modules_probe ... all` runs every pass for each entry and writes the
declaration dump, the checker tables, the IR after lowering, the IR after
borrow checking, the IR after async frame conversion, and the machine
section (or `machine skipped` after a failed pass) as one blob. CI runs it
under the leak checker. The single-stage modes stay available for local
debugging.

## Driver and bootstrap

`main.snx` calls `driver.run(args)`. `Compilation.load(input, prelude,
std_fallback, stage, require_main)` returns an owned compilation or a source
read error. Stage limits are declarations (0), signatures (1), bodies (2),
lowering (3), ownership (4), and async conversion (5). A failed pass prevents
later passes. Sources remain available for diagnostic rendering.

`build.program(options)` runs those passes, Axon, object serialization, and the
built-in linker (or the system linker with `--linker=system`).
`driver_paths.link_command` keeps executable and arguments separate. The
embedded prelude (`prelude_source.snx`, generated from `prelude.snx` by
`embed_prelude.snx`), host discovery, inspection serializers, and CLI
transport adapters are in the `driver*.snx` modules.

## Bootstrap

CMake builds stage 1 with the seed release and stage 2 with stage 1. Both
link with the built-in linker against the runtime capsule built from
`runtime/`, so the compiler can call runtime functions that the seed's own
runtime lacks. Stage 2 is the compiler that the tests use and that the
packages install. The `bootstrap` test builds stage 3 with stage 2 and
requires byte-identical compiler objects, then builds and runs the workbench.

The compiler source must build with the seed. To use a language feature in
the compiler that the seed does not have, release a compiler that has the
feature first, then update `scripts/seed.json` to that release.
