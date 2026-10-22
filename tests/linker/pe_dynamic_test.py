#!/usr/bin/env python3
"""Inspect dynamic PE imports, execute on Windows, check deterministic output."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile


def u(data, at, width):
    assert 0 <= at and at + width <= len(data)
    return int.from_bytes(data[at:at + width], "little")


def inspect(data, pic=False):
    assert data[:2] == b"MZ"
    pe = u(data, 60, 4)
    assert data[pe:pe + 4] == b"PE\0\0"
    assert u(data, pe + 4, 2) == 0x8664
    optional = pe + 24
    assert u(data, optional, 2) == 0x20B
    assert u(data, optional + 70, 2) & 0x160 == 0x160
    assert u(data, optional + 72, 8) == 8 * 1024 * 1024
    sections = []
    next_rva = 4096
    for i in range(u(data, pe + 6, 2)):
        at = optional + u(data, pe + 20, 2) + 40 * i
        size, rva, raw_size, raw_at = struct.unpack_from("<IIII", data, at + 8)
        flags = u(data, at + 36, 4)
        assert raw_at + raw_size <= len(data)
        assert flags & 0xA0000000 != 0xA0000000  # W^X.
        assert rva == next_rva and size > 0  # Every reserved virtual page is represented.
        next_rva = (rva + size + 4095) // 4096 * 4096
        sections.append((rva, size, raw_at, raw_size, flags))

    def offset(rva, width=1):
        for start, size, at, raw_size, _ in sections:
            if start <= rva and rva + width <= start + size:
                assert rva - start + width <= raw_size
                return at + rva - start
        raise AssertionError(("unmapped RVA", hex(rva), width))

    def cstring(rva):
        at = offset(rva)
        end = data.index(b"\0", at)
        offset(rva, end - at + 1)
        return data[at:end].decode("ascii")

    directory, size = struct.unpack_from("<II", data, optional + 112 + 8)
    iat, iat_size = struct.unpack_from("<II", data, optional + 112 + 12 * 8)
    assert size == 60 and iat_size == 40
    assert any(start <= iat < start + extent and flags & 0x80000000
               for start, extent, _, _, flags in sections)
    found = {}
    slots = {}
    for at in range(directory, directory + size - 20, 20):
        ilt, stamp, chain, name, first = struct.unpack_from("<IIIII", data, offset(at, 20))
        assert stamp == chain == 0 and ilt % 8 == first % 8 == 0
        dll = cstring(name)
        entries = []
        index = 0
        while u(data, offset(ilt + index * 8, 8), 8):
            hint_name = u(data, offset(ilt + index * 8, 8), 8)
            assert hint_name < 0x80000000 and hint_name % 2 == 0
            assert u(data, offset(hint_name, 2), 2) == 0
            symbol = cstring(hint_name + 2)
            assert u(data, offset(first + index * 8, 8), 8) == hint_name
            assert iat <= first + index * 8 < iat + iat_size
            entries.append(symbol)
            slots[symbol] = first + index * 8
            index += 1
        assert u(data, offset(first + index * 8, 8), 8) == 0
        found[dll] = entries
    assert data[offset(directory + size - 20, 20):offset(directory + size - 20, 20) + 20] == bytes(20)
    assert found == {"kernel32.dll": ["GetCurrentProcessId", "ExitProcess"], "msvcrt.dll": ["abs"]}
    # Every named import has a callable RIP-relative IAT thunk in executable code.
    thunks = {}
    for rva, size, at, raw_size, flags in sections:
        if not flags & 0x20000000:
            continue
        payload = data[at:at + min(size, raw_size)]
        for i in range(len(payload) - 5):
            if payload[i:i + 2] == b"\xff\x25":
                slot = rva + i + 6 + struct.unpack_from("<i", payload, i + 2)[0]
                thunks[slot] = rva + i
    assert set(slots.values()) <= set(thunks)
    reloc_rva, reloc_size = struct.unpack_from("<II", data, optional + 112 + 5 * 8)
    assert reloc_size >= 12
    # The fixture's absolute function pointer points at a thunk, with DIR64 fixup.
    base = u(data, optional + 24, 8)
    cursor = 0
    absolute = []
    while cursor < reloc_size:
        page, block_size = struct.unpack_from("<II", data, offset(reloc_rva + cursor, 8))
        assert page % 4096 == 0 and block_size >= 8 and block_size % 4 == 0
        for step in range(8, block_size, 2):
            entry = u(data, offset(reloc_rva + cursor + step, 2), 2)
            if entry:
                assert entry >> 12 == 10
                absolute.append(u(data, offset(page + (entry & 4095), 8), 8) - base)
        cursor += block_size
    assert cursor == reloc_size and absolute == ([] if pic else [thunks[slots["abs"]]])
    if pic:
        assert reloc_size == 12


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    args = parser.parse_args()
    probe = args.probe.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="_work-pe-dynamic-", dir=Path(__file__).parent) as tmp:
        directory = Path(tmp).resolve()
        snapshots = []
        for _ in range(2):
            result = subprocess.run([str(probe), str(directory)], capture_output=True, timeout=120)
            assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
            assert not result.stdout and not result.stderr
            pair = tuple((directory / name).read_bytes() for name in ("dynamic.exe", "dynamic-pic.exe", "dynamic-empty.exe"))
            inspect(pair[0])
            inspect(pair[1], pic=True)
            inspect(pair[2], pic=True)
            snapshots.append(pair)
        assert snapshots[0] == snapshots[1]
        if os.name == "nt":
            for name in ("dynamic.exe", "dynamic-pic.exe", "dynamic-empty.exe"):
                for _ in range(3):
                    result = subprocess.run([str(directory / name)], capture_output=True, timeout=10)
                    assert result.returncode == 42, (result.returncode, result.stdout, result.stderr)
                    assert not result.stdout and not result.stderr
        print("dynamic PE: DLL groups, named ILT/IAT, direct/address/indirect imports, BSS, DIR64, deterministic output passed")


if __name__ == "__main__":
    main()
