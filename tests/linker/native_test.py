#!/usr/bin/env python3
"""Inspect and run native images emitted by an already-built native_probe.

No external linker is invoked. Probe outputs stay in this directory's temporary
folders. Each probe also runs the Synx resolver/relocation negative fixtures.
"""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile


def uint(data, offset, width):
    assert offset >= 0 and offset + width <= len(data)
    return int.from_bytes(data[offset:offset + width], "little")


def verify_data(text, rodata, data, text_va, rodata_va, data_va, bss_va, call_offset):
    assert rodata == struct.pack("<II", 41, 42)
    assert uint(data, 0, 8) == text_va
    assert uint(data, 8, 8) == rodata_va + 4
    displacement = struct.unpack_from("<i", text, call_offset)[0]
    assert call_offset + 4 + displacement == 16
    assert text[16:18] == b"\x48\xb8"
    assert uint(text, 18, 8) == data_va + 8
    assert text[31:33] == b"\x48\xb9"
    assert uint(text, 33, 8) == bss_va


def verify_pe(data):
    assert data[:2] == b"MZ"
    pe = uint(data, 60, 4)
    assert data[pe:pe + 4] == b"PE\0\0"
    machine, count, stamp, symbols, symbol_count, optional_size, flags = struct.unpack_from("<HHIIIHH", data, pe + 4)
    assert (machine, count, stamp, symbols, symbol_count, optional_size) == (0x8664, 5, 0, 0, 0, 240)
    assert flags & 0x22 == 0x22
    optional = pe + 24
    assert uint(data, optional, 2) == 0x20B
    assert uint(data, optional + 32, 4) == 4096
    assert uint(data, optional + 36, 4) == 512
    assert uint(data, optional + 68, 2) == 3  # Console.
    assert uint(data, optional + 70, 2) & 0x160 == 0x160  # ASLR, high entropy, DEP.
    assert uint(data, optional + 72, 8) == 8388608
    assert uint(data, optional + 108, 4) == 16
    assert uint(data, optional + 112 + 8, 8) == 0  # No imports for this fixture.
    image_base = uint(data, optional + 24, 8)
    assert image_base == 0x140000000
    assert uint(data, optional + 16, 4) == 0x1000
    sections = {}
    for i in range(count):
        at = optional + optional_size + i * 40
        name = data[at:at + 8].rstrip(b"\0").decode("ascii")
        size, rva, raw_size, raw_at = struct.unpack_from("<IIII", data, at + 8)
        protection = uint(data, at + 36, 4)
        assert rva % 4096 == 0 and raw_at % 512 == 0 and raw_size % 512 == 0
        assert raw_at + raw_size <= len(data)
        assert uint(data, optional + 56, 4) >= rva + size
        sections[name] = (size, rva, raw_size, raw_at, protection)
    assert set(sections) == {".text", ".rdata", ".data", ".bss", ".reloc"}
    assert sections[".text"][4] & 0xE0000000 == 0x60000000
    assert sections[".rdata"][4] & 0xE0000000 == 0x40000000
    assert sections[".data"][4] & 0xE0000000 == 0xC0000000
    assert sections[".bss"][4] & 0xE0000000 == 0xC0000000
    assert sections[".bss"][0] == 8 and sections[".bss"][2:4] == (0, 0)
    def payload(name):
        size, _, _, offset, _ = sections[name]
        return data[offset:offset + size]
    verify_data(payload(".text"), payload(".rdata"), payload(".data"),
                image_base + sections[".text"][1], image_base + sections[".rdata"][1],
                image_base + sections[".data"][1], image_base + sections[".bss"][1], 5)
    reloc_rva, reloc_size = struct.unpack_from("<II", data, optional + 112 + 5 * 8)
    assert reloc_rva == sections[".reloc"][1] and reloc_size == sections[".reloc"][0]
    reloc = payload(".reloc")
    cursor = 0
    sites = []
    while cursor < len(reloc):
        page, size = struct.unpack_from("<II", reloc, cursor)
        assert page % 4096 == 0 and size >= 8 and size % 4 == 0
        assert cursor + size <= len(reloc)
        for at in range(cursor + 8, cursor + size, 2):
            entry = uint(reloc, at, 2)
            if entry:
                assert entry >> 12 == 10
                sites.append(page + (entry & 4095))
        cursor += size
    assert sites == sorted([sections[".text"][1] + 18, sections[".text"][1] + 33,
                            sections[".data"][1], sections[".data"][1] + 8])


def verify_elf(data):
    assert data[:7] == b"\x7fELF\x02\x01\x01"
    assert uint(data, 16, 2) == 2 and uint(data, 18, 2) == 62
    assert uint(data, 24, 8) == 0x401000
    assert uint(data, 32, 8) == 64 and uint(data, 40, 8) == 0
    assert uint(data, 52, 2) == 64 and uint(data, 54, 2) == 56
    count = uint(data, 56, 2)
    assert count == 6
    loads = []
    stack = None
    for i in range(count):
        kind, flags, offset, va, pa, file_size, memory_size, alignment = struct.unpack_from("<IIQQQQQQ", data, 64 + i * 56)
        assert va == pa
        if kind == 0x6474E551:
            stack = flags
            continue
        assert kind == 1 and alignment == 4096
        assert va % alignment == offset % alignment
        assert memory_size >= file_size and offset + file_size <= len(data)
        assert flags & 3 != 3  # No writable/executable segment.
        loads.append((flags, offset, va, file_size, memory_size))
    assert stack == 6
    assert loads[0] == (4, 0, 0x400000, 4096, 4096)
    text, rodata, initialized, bss = loads[1:]
    assert [x[0] for x in (text, rodata, initialized, bss)] == [5, 4, 6, 6]
    assert bss[3:] == (0, 8)
    def payload(segment):
        return data[segment[1]:segment[1] + segment[3]]
    verify_data(payload(text), payload(rodata), payload(initialized), text[2], rodata[2],
                initialized[2], bss[2], 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    args = parser.parse_args()
    probe = args.probe.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="_work-native-", dir=Path(__file__).parent) as temp:
        directory = Path(temp).resolve()
        snapshots = []
        for _ in range(2):
            result = subprocess.run([str(probe), str(directory)], capture_output=True, timeout=120)
            assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
            assert not result.stderr, result.stderr
            pe = (directory / "linked.exe").read_bytes()
            elf = (directory / "linked.elf").read_bytes()
            verify_pe(pe)
            verify_elf(elf)
            snapshots.append((pe, elf))
        assert snapshots[0] == snapshots[1], "image output changed between identical links"
        target = directory / ("linked.exe" if os.name == "nt" else "linked.elf")
        if os.name != "nt":
            target.chmod(0o755)
        for _ in range(3):
            result = subprocess.run([str(target)], capture_output=True, timeout=10)
            assert result.returncode == 42, (result.returncode, result.stdout, result.stderr)
            assert not result.stdout and not result.stderr
        print("native linker: resolution/range checks, PE+ELF headers/relocations, deterministic bytes, exit 42 passed")
        print("PE  " + hashlib.sha256(snapshots[0][0]).hexdigest())
        print("ELF " + hashlib.sha256(snapshots[0][1]).hexdigest())


if __name__ == "__main__":
    main()
