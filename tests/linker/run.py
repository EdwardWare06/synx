#!/usr/bin/env python3
"""Test an already-built selfhost/link/probe.snx executable.

Usage: python tests/linker/run.py <probe> [--archive <real-library>]...
Only this directory contains temporary outputs. No build-tree changes.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


MAGIC = b"!<arch>\n"


def member(name, payload, *, size=None, trailer=b"`\n", pad=b"\n"):
    assert len(name) <= 16
    header = (name.ljust(16) + b"0".ljust(12) + b"0".ljust(6)
              + b"0".ljust(6) + b"100644".ljust(8)
              + (str(len(payload)).encode() if size is None else size).ljust(10)
              + trailer)
    assert len(header) == 60
    return header + payload + (pad if len(payload) % 2 else b"")


def record(header, name, payload, metadata=False, prefix=0):
    return (f"{header} {header + 60 + prefix} {len(payload)} "
            f"{int(metadata)} {name.hex()} {payload.hex()}\n").encode()


def valid_cases():
    yield "empty", MAGIC, b"ok\n"
    a = member(b"first.o/", b"abc")
    b = member(b"second.o/", b"\x00\xff")
    yield "short", MAGIC + a + b, (b"ok\n" + record(8, b"first.o", b"abc")
                                  + record(8 + len(a), b"second.o", b"\x00\xff"))
    yield "raw-name", MAGIC + member(b"\xff.o/", b""), b"ok\n" + record(8, b"\xff.o", b"")
    yield "spaces", MAGIC + member(b"with spaces.o/", b"x"), b"ok\n" + record(8, b"with spaces.o", b"x")
    a = member(b"/", b"index-one")
    b = member(b"/", b"index-two")
    c = member(b"/SYM64/", b"index64")
    yield "indexes", MAGIC + a + b + c, (b"ok\n" + record(8, b"/", b"index-one", True)
        + record(8 + len(a), b"/", b"index-two", True)
        + record(8 + len(a) + len(b), b"/SYM64/", b"index64", True))
    for style, names in [("gnu", b"long object name.o/\nsecond object.o/\n"),
                         ("coff", b"long object name.o\x00second object.o\x00")]:
        split = names.index(b"second")
        a = member(b"//", names)
        b = member(b"/0", b"abc")
        c = member(f"/{split}".encode(), b"defg")
        yield style, MAGIC + a + b + c, (b"ok\n" + record(8, b"//", names, True)
            + record(8 + len(a), b"long object name.o", b"abc")
            + record(8 + len(a) + len(b), b"second object.o", b"defg"))
    names = b"late name.o/\n"
    a = member(b"/0", b"q")
    b = member(b"//", names)
    yield "table-after-use", MAGIC + a + b, (b"ok\n" + record(8, b"late name.o", b"q")
        + record(8 + len(a), b"//", names, True))
    for name in [b"long BSD member name.o", b"__.SYMDEF SORTED", b"__.SYMDEF_64 SORTED"]:
        padded = name + b"\x00" * 3
        payload = b"\x00\x01\xff"
        yield "bsd-" + name.hex(), MAGIC + member(f"#1/{len(padded)}".encode(), padded + payload), (
            b"ok\n" + record(8, name, payload, name.startswith(b"__.SYMDEF"), len(padded)))
    yield "bsd-short-index", MAGIC + member(b"__.SYMDEF", b"1234"), (
        b"ok\n" + record(8, b"__.SYMDEF", b"1234", True))
    yield "empty-table", MAGIC + member(b"//", b""), b"ok\n" + record(8, b"//", b"", True)
    # Decimal field has no spaces and requires all ten bytes.
    yield "zero-field", MAGIC + member(b"empty/", b"", size=b"0000000000"), b"ok\n" + record(8, b"empty", b"")


def invalid_cases():
    yield "bad-magic", b"BADMAGIC", b"error invalid 0\n"
    yield "thin", b"!<thin>\n", b"error unsupported 0\n"
    yield "trailer", MAGIC + member(b"a/", b"", trailer=b"??"), b"error invalid 66\n"
    yield "blank-size", MAGIC + member(b"a/", b"", size=b""), b"error invalid 56\n"
    yield "negative-size", MAGIC + member(b"a/", b"", size=b"-1"), b"error invalid 56\n"
    yield "size-junk", MAGIC + member(b"a/", b"", size=b"1 2"), b"error invalid 57\n"
    yield "large-size", MAGIC + member(b"a/", b"", size=b"9999999999"), b"error truncated 68\n"
    yield "bad-padding", MAGIC + member(b"a/", b"x", pad=b"!"), b"error invalid 69\n"
    yield "missing-padding", MAGIC + member(b"a/", b"x", pad=b""), b"error truncated 69\n"
    yield "empty-name", MAGIC + member(b"", b""), b"error invalid 8\n"
    yield "zero-in-name", MAGIC + member(b"a\x00b/", b""), b"error invalid 8\n"
    yield "missing-table", MAGIC + member(b"/0", b""), b"error invalid 8\n"
    yield "slash-extension", MAGIC + member(b"/<unknown>/", b""), b"error unsupported 8\n"
    a = member(b"//", b"first/\n")
    yield "duplicate-table", MAGIC + a + a, f"error invalid {8 + len(a)}\n".encode()
    yield "bad-table-offset", MAGIC + a + member(b"/99", b""), b"error invalid 68\n"
    yield "middle-of-name", MAGIC + a + member(b"/1", b""), b"error invalid 69\n"
    a = member(b"//", b"first")
    yield "unterminated-name", MAGIC + a + member(b"/0", b""), b"error truncated 68\n"
    a = member(b"//", b"first\n")
    yield "bad-gnu-terminator", MAGIC + a + member(b"/0", b""), b"error invalid 73\n"
    a = member(b"//", b"\x00")
    yield "empty-long-name", MAGIC + a + member(b"/0", b""), f"error invalid {8 + len(a)}\n".encode()
    yield "bsd-too-long", MAGIC + member(b"#1/99", b"abc"), b"error invalid 8\n"
    yield "bsd-bad-length", MAGIC + member(b"#1/x", b"abc"), b"error invalid 8\n"
    yield "bsd-empty", MAGIC + member(b"#1/0", b"abc"), b"error invalid 8\n"
    yield "bsd-embedded-zero", MAGIC + member(b"#1/3", b"a\x00b"), b"error invalid 69\n"
    # Every incomplete prefix must return an ordinary error, without a panic.
    complete = MAGIC + member(b"one/", b"abc") + member(b"two/", b"\x00\xff")
    for end in range(len(complete)):
        if end in (8, 72):  # Complete empty/one-member archives.
            continue
        yield f"prefix-{end}", complete[:end], None


def run(probe, directory, name, data):
    source = directory / "input.a"
    report = directory / "report.txt"
    source.write_bytes(data)
    report.unlink(missing_ok=True)
    result = subprocess.run([str(probe), str(source), str(report)],
                            capture_output=True, timeout=30)
    if result.returncode or result.stderr:
        raise AssertionError(f"{name}: exit {result.returncode}: {result.stderr!r}")
    return report.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("--archive", type=Path, action="append", default=[])
    args = parser.parse_args()
    probe = args.probe.resolve(strict=True)
    count = 0
    with tempfile.TemporaryDirectory(prefix="_work-", dir=Path(__file__).parent) as temp:
        directory = Path(temp).resolve()
        for name, data, expected in list(valid_cases()) + list(invalid_cases()):
            actual = run(probe, directory, name, data)
            if expected is None:
                assert actual.startswith(b"error "), (name, actual)
            else:
                assert actual == expected, (name, expected, actual)
            count += 1
        for path in args.archive:
            data = path.read_bytes()
            actual = run(probe, directory, str(path), data)
            assert actual.startswith(b"ok\n"), (path, actual)
            for line in actual.splitlines()[1:]:
                header, offset, size, metadata, name, payload = line.decode("ascii").split(" ")
                assert int(header) >= 8 and int(offset) >= int(header) + 60
                assert bytes.fromhex(payload) == data[int(offset):int(offset) + int(size)]
                assert bytes.fromhex(name) and metadata in ("0", "1")
            print(f"real archive: {path} ({len(actual.splitlines()) - 1} members)")
            count += 1
    print(f"linker reader: {count} cases passed")


if __name__ == "__main__":
    main()
