"""Compare match coverage with enumeration of finite Boolean products.

One compiler call checks all rejected cases; a second checks all accepted
cases through lowering and borrow checking. Sources remain in WORKDIR.
Usage: python tests/patterns/coverage.py SYNX WORKDIR
"""
import itertools
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    compiler = Path(sys.argv[1]).resolve()
    work = Path(sys.argv[2]).resolve()
    work.mkdir(parents=True, exist_ok=True)
    declarations = [
        "enum Product { Both(bool, bool) }",
        "enum Named { Both { bool a; bool b; } }",
        "struct Pair { bool a; bool b; }",
    ]
    sources = {True: declarations.copy(), False: declarations.copy()}
    expected_errors = set()
    count = 0
    values = list(itertools.product((True, False), repeat=2))
    patterns = list(itertools.product(("true", "false", "_"), repeat=2))
    shapes = [
        ("bool a, bool b", "(a,b)", lambda a, b: f"({a},{b})", ""),
        ("Product p", "p", lambda a, b: f"Product.Both({a},{b})", ""),
        ("Named p", "p", lambda a, b: f"Named.Both {{ a: {a}, b: {b} }}", ""),
        ("Pair p", "p", lambda a, b: f"Pair {{ a: {a}, b: {b} }}", ""),
        ("&const Product p", "p", lambda a, b: f"Product.Both({a},{b})", ""),
        ("Option<bool> a, Option<bool> b", "(a,b)",
         lambda a, b: f"(Some({a}),Some({b}))", "(None,_) | (_,None) => 0,"),
    ]
    for params, operand, render, extra in shapes:
        for subset in range(1 << len(patterns)):
            selected = [p for i, p in enumerate(patterns) if subset & (1 << i)]
            exhaustive = all(any(all(p == "_" or (p == "true") == v
                                     for p, v in zip(pattern, value))
                                 for pattern in selected) for value in values)
            # Joining all arms with | exercises alternative expansion while
            # preserving the same enumeration oracle.
            arms = " | ".join(render(*pattern) for pattern in selected)
            arms = (arms + " => 1,") if arms else ""
            text = (f"int case_{count}({params}) {{ return match {operand} {{ "
                    f"{extra} {arms} _ if false => 0 }}; }}")
            sources[exhaustive].append(text)
            if not exhaustive:
                expected_errors.add(len(sources[False]))
            count += 1

    env = dict(os.environ, SYNX_STD_PATH=str(ROOT / "std"))
    for accepted in (False, True):
        path = work / ("accepted.snx" if accepted else "rejected.snx")
        path.write_text("\n".join(sources[accepted]) + "\n", encoding="utf-8", newline="\n")
        result = subprocess.run([str(compiler), "check", str(path), "--json"],
                                env=env, capture_output=True, text=True,
                                encoding="utf-8", timeout=600)
        try:
            report = json.loads(result.stdout)
        except ValueError:
            sys.exit(f"invalid compiler response ({result.returncode})\n{result.stdout}\n{result.stderr}")
        diagnostics = report.get("diagnostics", [])
        if accepted:
            if result.returncode or diagnostics or not report.get("ok"):
                sys.exit(f"covered matches rejected:\n{result.stdout}\n{result.stderr}")
        else:
            got = [d["range"]["start"]["line"] for d in diagnostics if d.get("code") == "SNX0404"]
            if (result.returncode != 1 or len(got) != len(expected_errors)
                    or set(got) != expected_errors or len(diagnostics) != len(got)):
                sys.exit(f"coverage differs: expected {len(expected_errors)} gaps, got {len(got)}\n"
                         f"missing lines: {sorted(expected_errors - set(got))}\n"
                         f"extra lines: {sorted(set(got) - expected_errors)}\n{result.stdout}")
    print(f"match coverage: {count} Boolean-product cases agree with enumeration")


if __name__ == "__main__":
    main()
