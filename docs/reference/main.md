# Program entry

Defines the function called by runtime startup.

## Syntax

```text
main() { body }
void main() { body }
int main() { body }
```

## Explanation

The executable entry path initializes the runtime and calls the program's
`main`. Main has no parameters. Use `std.env.args()` for process arguments.
A void main completes with status zero. An int main supplies the process
status from its return value.

Build and run commands compile source to native machine code. `run` builds
the executable and launches it. It does not interpret source statements.
Expected compiler failures return status one. Runtime panic status is 101.

## Example

```synx
int main() {
    print("ready");
    return 0;
}
```

Output: `ready`. Process status: `0`.

## Notes

The current self-hosted compiler uses its built-in linker by default. Keep
the matching runtime capsule beside a distributed executable. Windows and
Linux packages have native validation; current macOS bootstrap and package
validation remain pending. See the installation and command guides.
