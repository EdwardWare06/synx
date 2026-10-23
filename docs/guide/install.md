# Installing Synx

Synx 0.1.0 is distributed as a package for each host. A package holds the
compiler, the runtime, and the standard library source. The compiler has its
own linker, so no C compiler, Visual Studio, or system linker is needed to
build programs.

| Host | Package | Requirements |
|------|---------|--------------|
| Windows | `synx-0.1.0-windows-x86_64.zip` | 64-bit Windows on x86-64 |
| Linux | `synx-0.1.0-linux-x86_64.tar.gz` | x86-64, glibc 2.34 or later |
| macOS | `synx-0.1.0-macos-x86_64.tar.gz` | macOS 10.15 or later; Rosetta 2 on Apple Silicon |

Synx makes x86-64 code only. On a Mac with Apple Silicon, the compiler and
the programs it builds run through Rosetta 2. If Rosetta 2 is not installed,
install it with `softwareupdate --install-rosetta --agree-to-license`.

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

## Linux and macOS

1. Extract the archive and run the installer. On macOS, use the file name
   `synx-0.1.0-macos-x86_64`.

   ```text
   tar -xzf synx-0.1.0-linux-x86_64.tar.gz
   cd synx-0.1.0-linux-x86_64
   ./install.sh
   ```

2. Open a new shell and run `synx version`. The output is
   `Synx 0.1.0 (linux)` or `Synx 0.1.0 (macos)`.

`install.sh` copies the package to `~/.local/share/synx` and creates the link
`~/.local/bin/synx`. If `~/.local/bin` is not on `PATH`, the installer adds
an `export PATH=...` line to the login profile of your shell: `~/.zprofile`
for zsh (the macOS default), `~/.bash_profile` for bash when that file
exists, and `~/.profile` in all other cases.

On macOS, a browser marks downloaded files as quarantined, and macOS does not
run quarantined programs that are not signed. The installer removes the
quarantine mark from the installed copy. If you run the compiler from the
extracted folder without the installer, remove the mark first:

```text
xattr -dr com.apple.quarantine synx-0.1.0-macos-x86_64
```

Options:

| Option | Effect |
|--------|--------|
| `--prefix DIR` | Install to *DIR*. |
| `--bindir DIR` | Create the `synx` link in *DIR*. |
| `--no-path` | Do not change the shell profile. |
| `--uninstall` | Remove the installed copy and the link. |

To uninstall:

```text
~/.local/share/synx/install.sh --uninstall
```

Give the same `--prefix` and `--bindir` that you used to install. The
`PATH` line in the shell profile stays. Remove it by hand if you do not want
it.

## Programs and the runtime library

A program built by Synx loads the Synx runtime library at startup. The
compiler copies the library next to each program that it builds:

| Host | Runtime library |
|------|-----------------|
| Windows | `synx_rt.dll` |
| Linux | `libsynx_rt.so` |
| macOS | `libsynx_rt.dylib` |

To run a program on another computer, copy the program and the runtime
library to the same folder. The other computer does not need Synx installed.

## Building from source

Building the compiler from source is necessary only to change the compiler.
The source build uses CMake 3.20 or later, a C and C++ compiler, and
Python 3. The compiler is written in Synx, so the build also needs a Synx
compiler to start from (the seed). By default, CMake downloads the release
named in `scripts/seed.json` and checks its SHA-256. To use a Synx compiler
that you already have, pass its executable to CMake:

```text
cmake -S . -B build -DSYNX_SEED=/path/to/synx
```

On Windows, give `libexec\synx.exe` from the installation, not
`bin\synx.cmd`.

The tools for each host:

- Windows: Visual Studio Build Tools with the C++ workload. Run
  `scripts\build-win.cmd Release`.
- Linux: `build-essential`, `cmake`, and `ninja-build`. Run
  `scripts/build-linux.sh Release`.
- macOS: the Xcode command line tools, `cmake`, and `ninja` (for example
  from Homebrew). Run `scripts/build-linux.sh Release`.

Each script builds the compiler and runs the test suite. To make an install
package, run `scripts\package-win.cmd` or `scripts/package-linux.sh`. The
package is written to `dist/packages`.

## Next

[Getting started](getting-started.md) builds and runs a first program.
