# Changelog

## 0.2.0

- Live diagnostics for unsaved files through `synx lsp`.
- Format Document and format on save through the compiler formatter.
- Hover, definitions, references, completion, argument hints, and rename.
- Canonical path mapping, including Windows short paths.
- Saved-file checks for older compilers.
- Syntax colors for doubled braces in strings.


## 0.1.0

- Syntax colors for Synx (`.snx`) files, with string interpolation, nested
  block comments, and doc comments.
- Comment, bracket, and indentation rules, and snippets.
- Compiler errors in the editor when a file is opened or saved, including
  errors in imported files.
- **Synx: Run File**, **Synx: Build File**, and **Synx: Check File**, with
  Run and Build buttons in the editor title bar.
- A status bar item with the compiler version, or a warning when the
  compiler is not found.
