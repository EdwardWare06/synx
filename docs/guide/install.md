# Installing Synx

Synx 0.1.0 is distributed as a package for each host. A package holds the
compiler, the runtime, and the standard library source. The compiler has its
own linker, so no C compiler, Visual Studio, or system linker is needed to
build programs.

| Host | Package | Requirements |
|------|---------|--------------|
| Windows | `synx-0.1.0-windows-x86_64.zip` | 64-bit Windows on x86-64 |
| Linux | `synx-0.1.0-linux-x86_64.tar.gz` | x86-64, glibc 2.34 or later |

There is no macOS package in this release. The macOS code generator and
object writer exist, but the macOS package has not been tested.

## Windows

1. Extract the zip file to any folder.
2. In the extracted folder, run `install.cmd`.
3. Open a new terminal and run:

   ```text
   synx version
   ```

   The output is `Synx 0.1.0 (windows)`.

`install.cmd` copies the package to `%LOCALAPPDATA%\Programs\Synx`. It adds
the `bin` folder to the user `PATH` and sets the user variable `SYNX_HOME`. It
does not need administrator rights.

Options:

| Option | Effect |
|--------|--------|
| `-Destination <dir>` | Install to *dir*. |
| `-NoPath` | Do not change `PATH` or `SYNX_HOME`. Run the compiler as `<dir>\bin\synx.cmd`. |
| `-Uninstall` | Remove the installed copy, its `PATH` entry, and `SYNX_HOME`. |

To uninstall, run the installer from a folder outside the installation:

```text
cd %USERPROFILE%
"%LOCALAPPDATA%\Programs\Synx\install.cmd" -Uninstall
```

If you used `-Destination`, give the same `-Destination` to `-Uninstall`.

The installer refuses to replace or remove a folder that does not hold a
Synx installation.

## Linux

1. Extract the archive and run the installer:

   ```text
   tar -xzf synx-0.1.0-linux-x86_64.tar.gz
   cd synx-0.1.0-linux-x86_64
   ./install.sh
   ```

2. Open a new shell and run `synx version`. The output is
   `Synx 0.1.0 (linux)`.

`install.sh` copies the package to `~/.local/share/synx` and creates the link
`~/.local/bin/synx`. If `~/.local/bin` is not on `PATH`, the installer adds
an `export PATH=...` line to `~/.profile`.

Options:

| Option | Effect |
|--------|--------|
| `--prefix DIR` | Install to *DIR*. |
| `--bindir DIR` | Create the `synx` link in *DIR*. |
| `--no-path` | Do not change `~/.profile`. |
| `--uninstall` | Remove the installed copy and the link. |

To uninstall:

```text
~/.local/share/synx/install.sh --uninstall
```

Give the same `--prefix` and `--bindir` that you used to install. The
`~/.profile` line stays. Remove it by hand if you do not want it.

## Programs and the runtime library

A program built by Synx loads the Synx runtime library at startup. The
compiler copies the library next to each program that it builds:

| Host | Runtime library |
|------|-----------------|
| Windows | `synx_rt.dll` |
| Linux | `libsynx_rt.so` |

To run a program on another computer, copy the program and the runtime
library to the same folder. The other computer does not need Synx installed.

## Building from source

Building the compiler from source is necessary only to change the compiler.
The source build uses CMake 3.20 or later and a C and C++ compiler:

- Windows: Visual Studio Build Tools with the C++ workload. Run
  `scripts\build-win.cmd Release`.
- Linux: `build-essential`, `cmake`, and `ninja-build`. Run
  `scripts/build-linux.sh Release`.

Each script builds the compiler and runs the test suite. To make an install
package, run `scripts\package-win.cmd` or `scripts/package-linux.sh`. The
package is written to `dist/packages`.

## Next

[Getting started](getting-started.md) builds and runs a first program.
