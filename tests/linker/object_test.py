#!/usr/bin/env python3
"""Run checked COFF/Mach-O readers against generated and malformed objects."""
import argparse
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    args = parser.parse_args()
    probe = args.probe.resolve(strict=True)
    result = subprocess.run([str(probe)], capture_output=True, timeout=120)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    assert not result.stderr, result.stderr
    match = re.fullmatch(rb"object readers: ([0-9]+) cases passed\r?\n", result.stdout)
    assert match and int(match.group(1)) > 700, result.stdout
    print(result.stdout.decode("ascii").strip())


if __name__ == "__main__":
    main()
