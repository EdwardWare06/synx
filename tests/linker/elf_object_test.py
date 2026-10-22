#!/usr/bin/env python3
"""Run ELF reader normalization, truncation and malformed-record fixtures."""
import argparse
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    args = parser.parse_args()
    result = subprocess.run([str(args.probe.resolve(strict=True))], capture_output=True, timeout=120)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    assert not result.stderr, result.stderr
    match = re.fullmatch(rb"ELF object reader: ([0-9]+) cases passed\r?\n", result.stdout)
    assert match and int(match.group(1)) > 900, result.stdout
    print(result.stdout.decode("ascii").strip())


if __name__ == "__main__":
    main()
