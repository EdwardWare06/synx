# Modules and imports

Loads source modules and binds names within the importing module.

## Syntax

```text
import path.to.module;
import path.to.item;
import path.to.{first, second};
pub import path.to.item;
pub declaration
```

## Explanation

Each source file is a module. A directory module uses `mod.snx`. Imports
resolve from the entry file's directory. Standard-library imports resolve
from the compiler's standard-library tree; `SYNX_STD_PATH` can override it.

A module import binds its short name and a private qualified route. Named
and grouped imports bind the selected items. Each importer has a separate
namespace. An import in one module does not make that route visible in
another module. `pub import` re-exports its explicit bindings.

Declarations are private unless exported with `pub`. Fields and inherent
methods have their own `pub` markers. Trait methods use their trait's
visibility. The prelude is available in each module; local declarations can
shadow prelude names.

## Example

```synx
import std.math;

main() {
    print("{math.abs(-4.0)}");
}
```

Output: `4.0`.

## Notes

The loader declares modules before resolving signatures, so mutually
dependent module signatures can be resolved. Import cycles still require
valid types and function bodies. An import is a compile-time name binding;
it does not run a module initializer.
