# Formatting source

```text
synx fmt main.snx
synx fmt src/main.snx src/helper.snx
synx fmt --check main.snx
synx fmt --stdout main.snx
synx fmt --stdin
```

The default command updates each named file. `--check` makes no changes and
returns status 1 if any file needs formatting. `--stdout` accepts one file
and writes the result to standard output. `--stdin` reads source through EOF
and writes the result to standard output. Combine `--stdin` with `--check`
to check piped source. Standard input has a 64 MiB limit.

Status 0 means success, 1 means a check found changes, and 2 means an option,
input, syntax, or output error. With several files, the command continues
after an error and returns the highest status. Each file is updated on its
own. Use `--` before a file name that starts with a hyphen.

## Style

- Four spaces per indentation level.
- Opening braces on the same line. Empty blocks use `{}`.
- One statement per line. `else` follows the preceding closing brace.
- Spaces around binary operators and after commas and colons.
- Tight member access, indexing, ranges, references, and type arguments.
- Short record literals and grouped imports stay on one line.
- Long argument, parameter, tuple, array, and type-argument lists wrap at
  their delimiters. The soft line limit is 100 columns. Long literals,
  comments, names, and expressions can exceed it.
- One blank line between top-level items. Consecutive imports stay together.
  Existing blank lines inside blocks are reduced to at most one.
- One final line ending. Empty input produces empty output.

For example:

```synx
int sum(int left, int right) {
    return left + right;
}

int main() {
    int total = sum(2, 3);
    print("total {total}");
    return 0;
}
```

Comments keep their text and order. Literal spelling stays exact, including
escapes, numeric bases, and whitespace inside string interpolation. The
formatter does not sort imports or rename items. It has no configuration
file or per-file suppression directive in this version.

The first line ending selects LF or CRLF for new layout. Each line comment
keeps its own line ending, since a doc comment's CR byte is part of its text.
Multiline comments and literal interiors keep their original bytes.

## Input and file updates

Only UTF-8 source with valid syntax can be formatted. Type errors and missing
imports do not prevent formatting. The command reparses its output and checks
every syntax field with source positions removed before it returns an edit.
If this comparison fails, the input stays unchanged.

File updates use a temporary file in the same directory and an atomic
replacement. The host adapter checks the original bytes before replacement.
An already formatted file keeps its modification time. Unix file permissions
and Windows access controls are retained. A symbolic link keeps its identity;
the command updates its resolved target. Replacement creates a new file
identity and does not retain a hard-link relationship.

## VS Code

The Synx extension uses the same formatter through `synx lsp`. Run **Format
Document**, or enable `editor.formatOnSave`. For a workspace with several
formatters, select Synx:

```json
{
    "[synx]": {
        "editor.defaultFormatter": "edwardware06.synx",
        "editor.formatOnSave": true
    }
}
```

Formatting uses the current unsaved buffer. It does not save the file.
Syntax errors produce no edits. The client discards a response if the buffer
changes while the request is running. Editor tab-width settings do not change
the fixed Synx style.
