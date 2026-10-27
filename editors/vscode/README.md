# Synx for Visual Studio Code

Language support for [Synx](https://github.com/EdwardWare06/synx), a
compiled language with ownership and borrowing, C-style syntax, and a
self-hosted compiler.

## Features

- Syntax colors for `.snx` files, including string interpolation
  (`"sum {a + b}"`), nested block comments, and `///` doc comments.
- Comment toggling, bracket matching, auto-closing pairs, and indentation.
- Snippets: `main`, `fn`, `struct`, `enum`, `trait`, `impl`, `if`,
  `ifelse`, `for`, `foreach`, `while`, `loop`, `match`, `matchopt`,
  `matchres`, `import`, and `print`.
- Live compiler errors for unsaved text, including edits to imported files.
- Type and function signature hover, go to definition, and find references.
- Completion for visible names, fields, methods, and imported modules.
- Function argument hints and symbol rename across files.
- Format Document and format on save through `synx fmt`, using four spaces
  and same-line braces. The compiler must include the formatter.
- **Synx: Run File** and **Synx: Build File** run the compiler in the
  terminal panel. The Run and Build buttons are in the editor title bar.
- **Synx: Check File** checks the current file.
- The status bar shows the compiler version. When the compiler is not
  found, it shows a warning; select it to download Synx or to set the path.

## Requirements

Install Synx from the
[releases page](https://github.com/EdwardWare06/synx/releases). The
extension finds the compiler in this order:

1. The `synx.compilerPath` setting.
2. The `SYNX_HOME` environment variable.
3. `PATH`.
4. The default install folder: `%LOCALAPPDATA%\Programs\Synx` on Windows,
   `~/.local/share/synx` on Linux and macOS.

Live features require a compiler with `synx lsp`. The extension detects
support automatically. Older compilers, including Synx 0.1.0, retain checks
on open and save. Restart the extension after replacing the compiler.

Rename requires a clean affected compilation and rejects name conflicts.
Expand shorthand bindings if the server cannot resolve each use.

## Settings

| Setting | Default | Effect |
|---------|---------|--------|
| `synx.compilerPath` | empty | Path to `synx`, `synx.cmd`, or `synx.exe`. Empty uses the search order above. |
| `synx.checkOnSave` | `true` | Check a file when you save it. |
| `synx.checkOnOpen` | `true` | Check a file when you open it. |
| `synx.checkOnChange` | `true` | Check unsaved edits with the language server. |
| `synx.checkDelay` | `300` | Milliseconds to wait after an edit. |
| `synx.entryPoints` | `[]` | Entry files relative to the workspace, or absolute paths. Empty uses the nearest ancestor `main.snx`. |
| `synx.buildArguments` | `[]` | Extra arguments for Run and Build, for example `["--release"]`. |

## Commands

| Command | Effect |
|---------|--------|
| Synx: Run File | Saves the file, then runs `synx run <file>`. |
| Synx: Build File | Saves the file, then runs `synx build <file>`. The executable is written beside the file. |
| Synx: Check File | Saves the file, then shows its errors. |
| Synx: Show Output | Shows the extension log: the compiler path and check failures. |

## License

MIT or Apache-2.0, at your option.
