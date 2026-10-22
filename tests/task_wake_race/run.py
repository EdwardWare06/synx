#!/usr/bin/env python3
"""Check deterministic wake races and the unwakeable-task diagnostic."""
import argparse
from pathlib import Path
import shutil
import subprocess


DIAGNOSTIC = b"local executor has no ready task"


def prepare(directory):
    source = Path(__file__).resolve().parents[2] / "std"
    target = directory / "baseline-std"
    shutil.copytree(source, target, dirs_exist_ok=True)
    task = target / "task.snx"
    text = task.read_text(encoding="utf-8")
    # Restore the exact old readiness/retention control flow for the baseline.
    start = text.index("        if (!wake.take_ready()) {")
    end = text.index("        match task.poll", start)
    old = ('        if (!wake.take_ready()) {\n'
           '            if (!wake.is_retained()) { panic("local executor has no ready task"); }\n'
           '            wake.wait(); continue;\n'
           '        }\n')
    text = text[:start] + old + text[end:]
    start = text.index("        if (!wake.take_ready()) {", start + len(old))
    end = text.index("        match task.poll", start)
    text = text[:start] + old + text[end:]
    changed = ('                if (!retained) {\n'
               '                    if (!self.signal.take_ready()) { panic("local executor has no ready task"); }\n'
               '                } else { self.signal.wait(); }')
    assert text.count(changed) == 1
    text = text.replace(changed, '                if (!retained) { panic("local executor has no ready task"); }\n                self.signal.wait();')
    task.write_text(text, encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path, nargs="?")
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--prepare", type=Path)
    args = parser.parse_args()
    if args.prepare:
        prepare(args.prepare.resolve())
        return
    assert args.probe
    for mode in ("send", "local", "executor", "silent-send", "silent-local", "silent-executor"):
        result = subprocess.run([str(args.probe.resolve(strict=True)), mode], capture_output=True, timeout=15)
        should_panic = args.baseline or mode.startswith("silent-")
        assert result.returncode == (101 if should_panic else 0), (mode, result.returncode, result.stdout, result.stderr)
        assert not result.stdout, (mode, result.stdout)
        if should_panic:
            assert DIAGNOSTIC in result.stderr, (mode, result.stderr)
        else:
            assert not result.stderr, (mode, result.stderr)
    print("wake race: 6 baseline failures verified" if args.baseline else "wake race: 3 handoffs and 3 dead-task diagnostics passed")


if __name__ == "__main__":
    main()
