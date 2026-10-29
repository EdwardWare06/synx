# Projects and dependencies

Myelin is the Synx package manager. Its commands run through `synx`.
Dependencies can come from local directories or Git repositories. Fetching Git
sources requires an installed `git` executable. Builds from fetched dependencies
and local path dependencies do not require Git.

## Create a project

```sh
synx new hello
cd hello
synx run
synx build --release
```

An application starts at `src/main.snx`. `synx new math --lib` creates a library
with `src/lib.snx`. The commands create source files, `synx.toml`, and `.gitignore`.
They do not create a Git repository. An existing destination is an error.

Project builds go to `build/debug/<name>` or `build/release/<name>`, with `.exe`
on Windows. `-o` selects another output. `synx run` uses the private native run
cache. Explicit source-file commands, such as `synx build example.snx`, remain
available.

Library packages compile from source as part of their consumers. Use
`synx check` to check a library by itself; the first version does not publish
precompiled library artifacts.

## Manifest

```toml
[package]
name = "hello"
version = "0.1.0"

[dependencies]
math = { path = "../math" }
noise = { git = "https://example.com/team/noise.git", tag = "v1.0.0" }
```

`package.name` is required. Names and dependency aliases use 1–64 ASCII letters,
digits, and underscores, starting with a letter or underscore. `std` and Synx
keywords are reserved.
`package.version` defaults to `0.1.0` and requires three unsigned decimal numbers
without leading zeroes. Version constraints and registry dependencies are future
work.

Optional `[bin]` and `[lib]` tables each accept `path`, an entry file relative to
the package directory. Entry paths cannot contain `.` or `..` components or
resolve outside the package. A dependency must have a library entry.

Dependency records accept these fields:

- `path`: a local directory containing `synx.toml`, relative to the declaring
  manifest or absolute. Changes to its source files remain live.
  `synx add --path` accepts a path from the current working directory and
  records it relative to the manifest directory.
- `git`: a Git URL. Myelin uses the installed Git credentials and standard
  streams. Remote helper syntax containing `::` is rejected.
- `rev`, `branch`, or `tag`: at most one selector. With no selector, Myelin
  resolves the repository's `HEAD` on the first fetch.
- `subdir`: a package directory inside the Git checkout. It cannot contain
  `.` or `..` components. Path dependencies inside Git packages must remain
  inside that checkout.

Exactly one of `path` or `git` is required. Git fields cannot accompany `path`.
The equivalent expanded form is:

```toml
[dependencies.noise]
git = "https://example.com/team/noise.git"
tag = "v1.0.0"
```

The first manifest format accepts a TOML subset: named tables, bare or quoted
keys, single-line basic or literal strings, comments, and inline dependency
tables. Arrays, multiline strings, dotted assignment keys, dates, numeric
values, and other TOML constructs are rejected. Strings support standard TOML
escapes, including `\u` and `\U`. Duplicate definitions and unknown fields are
errors. Input must be valid UTF-8 and at most 1 MiB. See the
[TOML specification](https://toml.io/en/v1.0.0) for string and comment syntax.

## Add and fetch dependencies

```sh
synx add math --path ../math
synx add noise --git https://example.com/team/noise.git --tag v1.0.0
synx fetch
synx build
```

`add` appends a dependency and preserves existing manifest bytes and comments.
It rejects an existing alias. `fetch` resolves the complete dependency graph,
fetches missing Git sources, and writes `synx.lock`. A project build or run also
performs this preparation. Git hooks and submodule initialization are disabled.
Myelin does not run package build scripts.

Git selectors resolve to full commit IDs in the lock file. Moving a branch or
tag does not change an existing pin. Commit `synx.lock` with the project. To
resolve new revisions in this first version, remove the lock file and run
`synx fetch`; this can change every Git pin.

```sh
synx fetch --locked
synx build --locked --offline
synx run --offline -- arguments-for-the-program
```

`--locked` rejects a missing or stale lock file and never changes it. It can
fetch a missing checkout at its pinned revision. `--offline` prevents Git
launches and requires cached Git sources and existing Git pins. Local graphs
can be resolved offline without an existing lock. Both flags can be combined.

Check and editor queries never fetch dependencies or update the lock file.
Run `synx fetch` when they report a missing or stale lock. `synx check` without
a source argument checks the project's application entry, or its library entry
when it has no application.

## Imports

```synx
import math.answer;

int main() {
    return answer();
}
```

The dependency alias names its library entry. `import math.geometry.Vec2`
resolves `geometry.snx` or `geometry/mod.snx` relative to that library's source
directory. Existing public/private declaration rules apply.

Each package resolves its own local modules and declared dependencies.
Transitive dependencies are visible through the package that declares them;
an application must declare its own dependency to import that package directly.
Different versions can coexist without merging their types or definitions.
Cycles in the package graph are errors. The graph is limited to 64 dependency
levels and 1,024 packages.

## Lock files and caches

`synx.lock` is generated JSON with format version 1. It records the package
graph, manifest SHA-256 hashes, and Git commit pins. It does not embed resolved
local checkout paths. Its package order follows sorted dependency aliases.
Manifest hashes normalize CRLF to LF for portable lock files.
Unchanged resolution preserves the lock file's modification time. Publication
uses a same-directory temporary file and atomic replacement.

Git checkouts live in a private user cache. `SYNX_MYELIN_CACHE` overrides it.
The default is `%LOCALAPPDATA%/Synx/cache/myelin` on Windows and
`~/.cache/synx/myelin` on Unix. Cache publication is serialized across processes.
An interrupted fetch can leave an unused `fetch-*` directory. Cached checkouts
are managed files; editing them is unsupported. Normal OS library dependencies
and Git's own authentication rules still apply.

Project operations use `.synx-myelin.lock` for synchronization. Exclude this
file from version control. Keep `synx.lock` in version control.
