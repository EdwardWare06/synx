# std.path

```synx
import std.path;
import std.path.{Path, Style};
```

This module holds `Path`, an owned path text with lexical operations. The
operations work on the text only. They do not access the file system and do
not resolve symbolic links. To use a `Path` with [std.fs](fs.md), pass
`path.as_str()`.

Each `Path` has a [Style](#style). The style sets the separators and the
root forms:

- `Style.Unix` uses `/` as the only separator. A root is a leading `/`.
- `Style.Windows` accepts `/` and `\` as separators. A root can be a drive
  (`C:`), a drive root (`C:\`), a current-drive root (a leading `\`), or a
  UNC root (`\\server\share`). A drive letter is an ASCII letter. Device
  namespaces such as `\\?\` and `\\.\` have no special handling.

## Types

### Style

```synx
enum Style { Unix, Windows }
```

The path syntax for the lexical operations.

### Path

```synx
struct Path
```

An owned UTF-8 path text and its [Style](#style). The fields are private.
Every operation that returns a path or a name returns new owned values.

### Path.from

```synx
Path from(&const str text)
```

Returns a `Path` that holds a copy of *text* with the style of the host,
from [host_style](#host_style).

### Path.with_style

```synx
Path with_style(&const str text, Style style)
```

Returns a `Path` that holds a copy of *text* with the given *style*. Use
this function to work with Windows paths on Linux or macOS, or the reverse.

### Path.as_str

```synx
&const str as_str(&const self)
```

Returns the stored text. The text is the same as the input. No separators
are changed.

### Path.is_absolute

```synx
bool is_absolute(&const self)
```

Returns `true` if the path does not depend on a current directory.

With `Style.Unix`, the path must start with `/`. With `Style.Windows`, the
path must start with a drive root such as `C:\` or `C:/`, or with two
separators such as `\\server\share`. A current-drive root such as `\data`
and a drive-relative path such as `C:data` are relative.

### Path.join

```synx
Path join(&const self, &const str child)
```

Returns a new path that adds *child* to this path. The result has the style
of this path.

- If *child* is absolute, the result is *child*.
- With `Style.Windows`, if *child* starts with a drive such as `D:`, the
  result is *child*.
- With `Style.Windows`, if *child* starts with one separator, the result is
  the drive of this path, if it has one, followed by *child*.
- If *child* is empty, the result is a copy of this path.
- In other cases, the result is this path, then `/`, then *child*. The `/`
  is not added if this path is empty, ends with a separator, or is a bare
  drive such as `C:`.

The function keeps the separators that are already in the text.

### Path.file_name

```synx
String? file_name(&const self)
```

Returns the last component of the path. Trailing separators are ignored.
Returns `None` if the path is empty, if the path is only a root, or if the
last component is `.` or `..`.

### Path.parent

```synx
Path? parent(&const self)
```

Returns the path without its last component and without the separators
before that component. Trailing separators are ignored. Returns `None` if
the path is empty or is only a root. A relative path with one component
returns `Some` of an empty path. A root stays in the result, so the parent
of `/a` is `/`.

The function does not interpret `.` and `..`. The parent of `a/..` is `a`.
The result keeps the separators of the original text.

### Path.extension

```synx
String? extension(&const self)
```

Returns the text after the last `.` in [file_name](#pathfile_name), without
the dot. Returns `None` if there is no file name or no dot. Returns `None`
if the only dot is the first character of the name, as in `.bashrc`.
Returns `Some("")` if the name ends with a dot, as in `file.`.

### Path.normalize

```synx
Path normalize(&const self)
```

Returns a lexical normal form of the path:

- Repeated separators and `.` components are removed.
- A `..` component removes the component before it, if that component is
  not also `..`.
- In a relative path, a `..` that has nothing to remove stays in the result.
  In a rooted path, it is removed, so the result cannot go above the root.
- All separators are written as `/`, also in the root.
- A path that becomes empty is written as `.`. An empty path stays empty.

The result can differ from the target in the file system when the path
contains symbolic links.

## Functions

### host_style

```synx
Style host_style()
```

Returns `Style.Windows` when the program runs on Windows. Returns
`Style.Unix` on Linux and macOS.

## Example

```synx
import std.path.{Path, Style};

void describe(&const Path path) {
    print("path: {path.as_str()}");
    print("  absolute: {path.is_absolute()}");
    match path.file_name() { Some(name) => print("  file name: {name}"), None => print("  file name: none") }
    match path.extension() { Some(ext) => print("  extension: {ext}"), None => print("  extension: none") }
    match path.parent() { Some(parent) => print("  parent: {parent.as_str()}"), None => print("  parent: none") }
    print("  normalized: {path.normalize().as_str()}");
}

main() {
    var base = Path.with_style("/srv/app", Style.Unix);
    var report = base.join("logs/../data/report.tar.gz");
    describe(&const report);
    var root = Path.with_style("/", Style.Unix);
    describe(&const root);

    var home = Path.with_style("C:\\Users\\ada", Style.Windows);
    var notes = home.join("..\\bob\\notes.txt");
    describe(&const notes);
    print(home.join("D:\\other").as_str());
    print(home.join("\\temp").as_str());
}
```

Output:

```text
path: /srv/app/logs/../data/report.tar.gz
  absolute: true
  file name: report.tar.gz
  extension: gz
  parent: /srv/app/logs/../data
  normalized: /srv/app/data/report.tar.gz
path: /
  absolute: true
  file name: none
  extension: none
  parent: none
  normalized: /
path: C:\Users\ada/..\bob\notes.txt
  absolute: true
  file name: notes.txt
  extension: txt
  parent: C:\Users\ada/..\bob
  normalized: C:/Users/bob/notes.txt
D:\other
C:\temp
```

The program gives the same output on every host, because each path has an
explicit style.
