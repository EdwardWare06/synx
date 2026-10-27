# Installing Synx

Synx 0.2.0 is distributed as a package for each host. A package holds the
compiler, the runtime, and the standard library source. The compiler has its
own linker, so no C compiler, Visual Studio, or system linker is needed to
build programs.

| Host | Package | Requirements |
|------|---------|--------------|
| Windows | `synx-0.2.0-windows-x86_64-setup.exe` or `synx-0.2.0-windows-x86_64.zip` | 64-bit Windows on x86-64 |
| Linux | `synx-0.2.0-linux-x86_64.tar.gz` | x86-64, glibc 2.34 or later |
| macOS | `synx-0.2.0-macos-x86_64.tar.gz` | macOS 10.15 or later; Rosetta 2 on Apple Silicon |

Synx makes x86-64 code only. On a Mac with Apple Silicon, the compiler and
the programs it builds run through Rosetta 2. If Rosetta 2 is not installed,
install it with `softwareupdate --install-rosetta --agree-to-license`.

## Windows

### Setup program

1. Run `synx-0.2.0-windows-x86_64-setup.exe`.
2. Select the install folder. The default is `%LOCALAPPDATA%\Programs\Synx`.
3. Keep **Add Synx to my PATH** selected, then select **Install**.
4. Open a new terminal and run `synx version`. The output is
   `Synx 0.2.0 (windows)`.

The setup program installs for the current user and does not need
administrator rights. **Add Synx to my PATH** adds the `bin` folder to the
user `PATH` and sets the user variable `SYNX_HOME`. The Start menu folder
**Synx** has a command prompt that can run `synx` without a change to `PATH`.

The setup program is not signed. Windows SmartScreen can show "Windows
protected your PC". To continue, select **More info**, then **Run anyway**.

To upgrade, run the setup program of the new version. It replaces the
installed copy, including a copy installed from the zip file.

To uninstall, open **Settings > Apps > Installed apps**, find **Synx**, and
select **Uninstall**. The uninstaller removes the `PATH` entry and
`SYNX_HOME`.

### Zip file

1. Extract the zip file to any folder.
2. In the extracted folder, run `install.cmd`.
3. Open a new terminal and run:

   ```text
   synx version
   ```

   The output is `Synx 0.2.0 (windows)`.

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
   `synx-0.2.0-macos-x86_64`.

   ```text
   tar -xzf synx-0.2.0-linux-x86_64.tar.gz
   cd synx-0.2.0-linux-x86_64
   ./install.sh
   ```

2. Open a new shell and run `synx version`. The output is
   `Synx 0.2.0 (linux)` or `Synx 0.2.0 (macos)`.

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
xattr -dr com.apple.quarantine synx-0.2.0-macos-x86_64
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

## Visual Studio Code

The [Synx extension](https://marketplace.visualstudio.com/items?itemName=edwardware06.synx)
adds syntax colors, snippets, and compiler errors in the editor, and Run and
Build buttons. In VS Code, open the Extensions view, search for **Synx**,
and select **Install**. Or run:

```text
code --install-extension edwardware06.synx
```

The extension finds the compiler through the `synx.compilerPath` setting,
`SYNX_HOME`, `PATH`, or the default install folder. Its source is in
`editors/vscode/`.

Extension 0.2.0 adds live diagnostics, completion, navigation, signature help,
rename, and formatting. To install the VSIX attached to the 0.2.0 release:

```text
code --install-extension synx-0.2.0.vsix --force
```

Reload VS Code after the update. Marketplace publishing is separate from
the compiler release.

## Running and distributing programs

`synx run program.snx` keeps native outputs in a private per-user cache.
The source folder contains only the files you put there.

`synx build program.snx` writes one executable with the Synx runtime
included. Copy that file to another computer with the same supported OS
and architecture. The other computer needs no Synx installation or Synx
runtime sidecar. Normal OS libraries remain required; Linux programs need
glibc 2.34 or later.

`--shared-runtime` retains the separate runtime-library form when needed.
See [the command reference](commands.md#run) for cache paths and options.

## Building from source

Building the compiler from source is necessary only to change the compiler.
The source build uses CMake 3.20 or later, a C and C++ compiler, and
Python 3. The compiler is written in Synx, so the build also needs a Synx
compiler to start from (the seed). By default, CMake downloads the release
named in `scripts/seed.json` (0.2.0) and checks its SHA-256. To use a Synx
0.2.0 or later compiler that you already have, pass its executable to CMake:

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
