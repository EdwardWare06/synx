# Synx

Synx is a programming language I designed and built from scratch, along with
its compiler, standard library, and linker. It compiles straight to native
x86-64 programs on Windows, Linux, and macOS. Memory is managed with ownership and
borrowing, there is no garbage collector, and the syntax looks like C.

```synx
enum Lift { Bench, Squat, Deadlift }

struct Set {
    Lift lift;
    int reps;
    int pounds;

    int volume(&const self) {
        self.reps * self.pounds
    }
}

&const str name(Lift lift) {
    match lift {
        Lift.Bench => "bench",
        Lift.Squat => "squat",
        Lift.Deadlift => "deadlift",
    }
}

main() {
    Vec<Set> week = Vec.new();
    week.push(Set { lift: Lift.Bench, reps: 5, pounds: 185 });
    week.push(Set { lift: Lift.Squat, reps: 5, pounds: 225 });
    week.push(Set { lift: Lift.Deadlift, reps: 3, pounds: 315 });

    int total = 0;
    for (set in &const week) {
        total += set.volume();
        print("{name(set.lift)}: {set.reps} x {set.pounds} lb");
    }
    print("total volume: {total} lb");
}
```

```text
bench: 5 x 185 lb
squat: 5 x 225 lb
deadlift: 3 x 315 lb
total volume: 2995 lb
```

## Why I built it

I wanted to challenge myself and really understand how languages and
compilers work under the hood. In class I've only worked with C, Java, and
Python, and I never got to see how a modern compiler pipeline works, the
HIR -> MIR -> LLVM kind of setup that newer languages use. So I built every
stage myself.

I also wanted to take the parts I liked most from each language I've used
and put them into one language. The C-style declarations come from C and
Java. The safety comes from Rust's ownership model. I wanted the quick,
readable feel of Python too, so types are inferred where it makes sense,
and strings take `{expressions}` directly. The long-term goal is an
all-in-one language that works for both backend and frontend work, with some
of my own interests and personality built into it.

## Install

The easiest way to get Synx is a package from the
[Releases](../../releases) page. Each package has the compiler, the runtime,
and the standard library. Synx links programs with its own linker, so you
don't need Visual Studio, a C compiler, or a system linker.

| Host | Package |
|------|---------|
| Windows x86-64 | `synx-0.1.0-windows-x86_64-setup.exe` or `synx-0.1.0-windows-x86_64.zip` |
| Linux x86-64 (glibc 2.34+) | `synx-0.1.0-linux-x86_64.tar.gz` |
| macOS 10.15+ (Rosetta 2 on Apple Silicon) | `synx-0.1.0-macos-x86_64.tar.gz` |

Windows: run the setup program, or extract the zip and run `install.cmd`.

Linux (on macOS, use `macos` in place of `linux`):

```text
tar -xzf synx-0.1.0-linux-x86_64.tar.gz
cd synx-0.1.0-linux-x86_64
./install.sh
```

Then open a new terminal and check it:

```text
synx version
```

The [install guide](docs/guide/install.md) covers install options and how
to uninstall.

## Quick start

Save this as `hello.snx`:

```synx
main() {
    print("Hello from Synx");
}
```

Run it:

```text
synx run hello.snx
```

`synx build hello.snx -o hello` makes an executable instead. The runtime
library (`synx_rt.dll`, `libsynx_rt.so`, or `libsynx_rt.dylib`) gets copied next to it. Copy
both files together and the program runs on another machine without Synx
installed. `synx check` checks a file without building it, and `synx about`
shows information about the build.

## How it's built

Everything between the source file and the executable is part of this
project:

- **Front end:** lexer, parser, module loader, and type checker with
  generics, traits, and type inference.
- **Dendrite:** the intermediate representation. The borrow checker, move
  checks, and async conversion all run on it.
- **Axon:** the code generator. It does register allocation, encodes x86-64
  instructions itself, and writes ELF, COFF, and Mach-O object files.
- **Linker:** a built-in linker that writes Windows, Linux, and macOS
  executables.
- **Standard library:** written in Synx. String, Vec, HashMap, sorting,
  files, paths, text, math, threads, channels, and async tasks are all Synx
  code. A small C runtime handles the things that have to talk to the OS:
  startup, memory, I/O, threads, and clocks.

The compiler is self-hosted. I started with a compiler in C++, then moved
the whole compiler over to Synx and retired the C++ version after 0.1.0. Now
the previous Synx release builds the compiler, that compiler builds itself,
and the output has to match byte for byte between stages. The packages ship
the compiler that was compiled by itself.

The component names come from my interest in neuroscience. A dendrite takes
in signals for a neuron and an axon sends them out, so Dendrite is where the
program comes in and Axon is where machine code goes out. Myelin speeds up
signals along an axon, and that's the name saved for the package manager.

The examples have more of me in them. The tutorial uses songs I listen to
and gym math in pounds: sets, reps, plates, and adding weight each set. There are also a few small details involving one of my favorite video games hidden around the toolchain if you go looking.

## Building from source

You only need this if you want to change the compiler. The build downloads
the Synx 0.1.0 release to compile the compiler with. To use a Synx you
already have, pass `-DSYNX_SEED=<path to the synx executable>` to CMake. A C
compiler builds the runtime and a C++ compiler builds the test programs, and
Python 3 runs the tests.

- Windows: Visual Studio Build Tools with the C++ workload, then
  `scripts\build-win.cmd Release`
- Linux: `build-essential`, `cmake`, and `ninja-build`, then
  `scripts/build-linux.sh Release`
- macOS: the Xcode command line tools, `cmake`, and `ninja`, then
  `scripts/build-linux.sh Release`

Each script builds the compiler and runs the full test suite.
`scripts\package-win.cmd` and `scripts/package-linux.sh` make the install
packages.

## Status and limits

Synx is at 0.1.0. It's a working language, but it's still early.

- Windows, Linux, and macOS on x86-64 are tested. On Apple Silicon Macs,
  Synx and the programs it builds run through Rosetta 2.
- No ARM64 code generation yet.
- It builds command-line programs. There are no GUI or web libraries yet,
  so the frontend part of the goal is still ahead.
- Trait objects only work through references (`&dyn Trait`).
- Values shared between threads with `Arc` or sent over channels have to own
  their data. They can't hold references.
- `?.` is reserved. The compiler parses it but rejects it during code
  generation, so use `match` or `??` for now.
- User types can't overload indexing.
- The Myelin package manager is planned, not built.

## Documentation

- [Getting started](docs/guide/getting-started.md) and the
  [tutorial](docs/guide/tutorial/01-first-steps.md), for people coming from
  Java or Python
- [Command reference](docs/guide/commands.md)
- [Language reference](docs/reference/index.md)
- [Standard library](docs/library/index.md)
- [All documentation](docs/index.md)

## A note on AI

I don't know everything about languages and compilers, and when I started
I had no clue where to even begin. I relied fairly heavily on AI, mainly Claude and ChatGPT, to point me where I needed to go. I chose the design and
directed the work, but AI did code portions of the project when I couldn't, especially in the compiler stages, and a lot of the documentation. Through all of this I've learned much, much more about how languages, compilers, and linkers work, and I plan to keep working on Synx with as little AI as possible.

## License

Synx is licensed under either of

- [Apache License, Version 2.0](LICENSE-APACHE)
- [MIT License](LICENSE-MIT)

at your option. Unless you explicitly state otherwise, any contribution
intentionally submitted for inclusion in Synx by you, as defined in the
Apache-2.0 license, shall be dual licensed as above, without any additional
terms or conditions.

## Author

Created by Edward Ware.
