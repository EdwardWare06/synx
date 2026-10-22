# Block expression

Groups statements and provides a scope and optional value.

## Syntax

```text
{ statements [final_expression] }
return [expression];
```

## Explanation

Declarations inside a block are local to it. Statements execute in order.
The last expression supplies the block's value when it has no semicolon.
A block without a final value has type `void`. A semicolon discards an
expression's result while preserving the expression's side effects.

Normal exits run the cleanup required for the scopes they leave. `return`
leaves the function after evaluating its result. `break` and `continue`
leave the relevant loop scopes. Error propagation with `?` can leave the
function. Owned values transferred into the result are not destroyed with
their old bindings.

Borrowed temporary owners remain alive until the enclosing block ends.
While conditions have an iteration scope through the loop body, which ends
before reevaluation and on each exit. Lifetime checks also run at scope
ends; a retained reference cannot keep dead storage alive.

## Example

```synx
main() {
    int total = {
        int left = 2;
        int right = 3;
        left + right
    };
    print("{total}");
}
```

Output: `5`.

## Notes

`return`, `break`, and `continue` are expressions which do not produce a
normal continuation value. They can appear in match arms. A panic ends the
process and does not perform block cleanup.
