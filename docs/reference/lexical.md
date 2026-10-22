# Source text and literals

Defines tokens, comments, and literal values.

## Syntax

```text
// line comment
/* nested /* block */ comment */
/// documentation comment
42  0xFF  0b1010  1_000  42L  1.5f  1.5  2e10
'a'  '\n'  '\u{1F600}'  "text"  true  false
```

## Explanation

Synx source uses UTF-8. Whitespace and comments separate tokens. Names are
case-sensitive. ASCII names start with a letter or `_` and continue with
letters, digits, or `_`. The current lexer also accepts non-ASCII bytes
in names; it does not implement a Unicode identifier-category standard.

An unsuffixed integer literal obtains its type from context and otherwise
defaults to `int`. `L` selects `long`. A floating literal defaults to
`double`; `f` selects `float`. An integer literal must fit its inferred
type in both arithmetic modes. Signed minima are accepted. An explicit
literal cast can request truncation.

Character and string escapes include `\n`, `\t`, `\r`, `\0`, `\\`, `\'`,
`\"`, `\{`, `\}`, and `\u{hex}`. Unicode escapes require a scalar value,
excluding surrogate code points. A plain string literal has type `&const str`.

## Example

```synx
main() {
    long total = 1_000L;
    byte mask = 0x0F;
    print("{total + mask as long}");
}
```

Output: `1015`.

## Notes

Reserved words include declaration, control-flow, and primitive-type names;
`once` is reserved. `send` is a contextual modifier, so it can also be a
name. Names such as `String`, `Option`, and `Vec` come from the prelude.
See [Strings](strings.md) for interpolation and [Operators](operators.md)
for token precedence.
