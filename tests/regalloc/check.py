"""Register allocation checks on the machine dump of the Synx compiler.

Usage: python tests/regalloc/check.py SYNX

Each case compiles a small program with `synx modules FILE --machine` and
checks the registers that Axon assigns to named locals of one function.
Register numbers are x86-64 encodings: RBX is 3, R8-R15 are 8-15, and -1
means a stack slot. Values live across calls must use callee-saved
registers (RBX, R12-R15).
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
RBX, R12 = 3, 12
checks = failures = 0


def check(condition, label):
    global checks, failures
    checks += 1
    if not condition:
        failures += 1
        print(f"FAIL {label}")


def machine(synx, source, function):
    """Returns ({local name: register}, [register by local index], saved) for one function."""
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "registers.snx"
        path.write_text(source, encoding="utf-8")
        result = subprocess.run([synx, "modules", str(path), "--machine"], capture_output=True,
                                encoding="utf-8", errors="replace")
    if result.returncode != 0:
        raise RuntimeError(result.stdout + result.stderr)
    dump = result.stdout
    ir_part, _, machine_part = dump.partition("\nmachine ")
    # IR section: f#N "name" ... then locals "  _K "type" "name" ...".
    index, names = None, []
    for line in ir_part.splitlines():
        header = re.match(r'f#(\d+) "([^"]*)" ', line)
        if header:
            if index is not None:
                break
            if header.group(2) == function:
                index = int(header.group(1))
            continue
        local = re.match(r'  _(\d+) "[^"]*" "([^"]*)" ', line)
        if index is not None and local:
            names.append(local.group(2))
    if index is None:
        raise RuntimeError(f"no function {function}")
    registers, saved, inside = [], [], False
    for line in machine_part.splitlines():
        header = re.match(r"f#(\d+) saved=\[([\d ]*)\]", line)
        if header:
            inside = int(header.group(1)) == index
            if inside:
                saved = [int(x) for x in header.group(2).split()]
            continue
        local = re.match(r"  _(\d+) reg=(-?\d+)", line)
        if inside and local:
            registers.append(int(local.group(2)))
    by_name = {name: registers[i] for i, name in enumerate(names) if name}
    return by_name, registers, saved


def callee_saved(register):
    return register == RBX or register >= R12


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    synx = str(Path(sys.argv[1]).resolve())
    os.environ.setdefault("SYNX_STD_PATH", str(ROOT / "std"))

    # Loop counters live across a call, so they need callee-saved registers.
    named, registers, _ = machine(synx, "int inc(int x) { return x + 1; } int main() { int sum = 0; int i = 0; "
                                        "while (i < 4) { sum += inc(i); i += 1; } return sum; }", "main")
    check(registers[0] == -1, "loops: the return slot stays on the stack")
    check(callee_saved(named["sum"]), "loops: sum uses a callee-saved register")
    check(callee_saved(named["i"]), "loops: i uses a callee-saved register")
    check(named["sum"] != named["i"], "loops: sum and i use different registers")

    # Address-taken values and aggregates stay in memory.
    named, _, _ = machine(synx, "int main() { int x = 1; &int r = &x; *r = 2; int[2] a = [1, 2]; "
                                "usize i = 1; return a[i] + x; }", "main")
    check(named["x"] == -1, "addresses: x is address-taken")
    check(named["r"] == -1, "addresses: r stays on the stack")
    check(named["a"] == -1, "addresses: the array stays on the stack")
    check(named["i"] >= 0, "addresses: the index gets a register")

    # Parameters that are live together get different registers.
    _, registers, _ = machine(synx, "int f(int a, int b, int c) { int x = a + 1; return x + b + c; } "
                                    "main() { f(1, 2, 3); }", "f")
    check(all(r >= 0 for r in registers[1:4]), "parameters: a, b, and c get registers")
    check(len(set(registers[1:4])) == 3, "parameters: a, b, and c do not share registers")

    # Under pressure some values spill; the rest use callee-saved registers.
    named, _, saved = machine(synx, "struct D { int x; } impl Drop for D { void drop(&self) { print(\"drop\"); } } "
                                    "int main() { int a=1; int b=2; int c=3; int d=4; int e=5; int f=6; int g=7; "
                                    "{ var owned = D { x: 1 }; } return a+b+c+d+e+f+g; }", "main")
    values = [named[n] for n in "abcdefg"]
    check(sum(1 for r in values if r < 0) >= 2, "pressure: at least two values spill")
    check(any(r >= 0 for r in values), "pressure: some values get registers")
    check(all(callee_saved(r) for r in values if r >= 0), "pressure: register values are callee-saved")
    check(len(saved) <= 5, "pressure: at most five saved registers")

    # A System V C call with 14 arguments compiles; only the link of the
    # missing C function fails.
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "c_call.snx"
        path.write_text("extern \"C\" { int many(int a, int b, int c, int d, int e, int f, double g, double h, "
                        "double i, double j, double k, double l, double m, double n); } "
                        "int main() { return unsafe { many(1,2,3,4,5,6,1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0) }; }",
                        encoding="utf-8")
        output = Path(folder) / "c_call"
        result = subprocess.run([synx, "build", str(path), "-o", str(output), "--keep-obj"],
                                capture_output=True, encoding="utf-8", errors="replace")
        text = result.stdout + result.stderr
        if sys.platform == "win32":
            # The Windows C ABI path supports four register arguments so far.
            check("error[SNX0601]" in text, f"C call: Windows reports the argument limit\n{text}")
        else:
            objects = list(Path(folder).glob("c_call.o*"))
            check(bool(objects), f"C call: the object file was written\n{text}")
            check("error[" not in text and "many" in text, f"C call: only linking fails\n{text}")

    print(f"regalloc: {checks} checks, {failures} failures")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
