"""Inspect a built-in-linker dynamic ELF and optionally run it.

Usage: python elf_dynamic_test.py PROGRAM --expect-library libsynx_rt.so
       [--expect-library libc.so.6] [--expect-exit 42]
No third-party ELF parser or system linker is required.
"""

import argparse
import pathlib
import struct
import subprocess


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def inspect(path):
    data = path.read_bytes()
    require(data[:7] == b"\x7fELF\x02\x01\x01", "ELF64 little-endian header")
    fields = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    kind, machine, version, entry, phoff, shoff, flags, ehsize, phsize, phnum, *_ = fields
    require((kind, machine, version, ehsize, phsize) == (2, 62, 1, 64, 56), "ELF target")
    require(phoff + phsize * phnum <= len(data), "program header extent")
    headers = [struct.unpack_from("<IIQQQQQQ", data, phoff + i * phsize) for i in range(phnum)]
    loads = [h for h in headers if h[0] == 1]
    require(loads, "PT_LOAD missing")
    require(all(h[1] & 3 != 3 for h in loads), "writable executable segment")
    for h in loads:
        _, protection, offset, address, physical, filesz, memsz, alignment = h
        require(filesz <= memsz and offset + filesz <= len(data), "LOAD extent")
        require(alignment == 4096 and offset % alignment == address % alignment, "LOAD alignment")
    require(any(h[1] & 1 and h[3] <= entry < h[3] + h[6] for h in loads), "entry outside executable load")
    for i, left in enumerate(loads):
        for right in loads[i + 1:]:
            require(left[3] + left[6] <= right[3] or right[3] + right[6] <= left[3], "LOAD overlap")

    def unique(kind):
        matching = [h for h in headers if h[0] == kind]
        require(len(matching) == 1, f"expected one program header {kind}")
        return matching[0]

    def at(address, size):
        for h in loads:
            if h[3] <= address and address + size <= h[3] + h[5]:
                return h[2] + address - h[3]
        raise AssertionError("metadata address outside file-backed LOAD")

    interp = unique(3)
    require(data[interp[2]:interp[2] + interp[5]] == b"/lib64/ld-linux-x86-64.so.2\0", "interpreter")
    stack = unique(0x6474E551)
    require(stack[1] == 6, "GNU stack must be RW")
    phdr = unique(6)
    require(phdr[2] == phoff and phdr[5] == phnum * phsize, "PT_PHDR extent")
    dynamic = unique(2)
    require(dynamic[1] == 6 and dynamic[5] % 16 == 0, "PT_DYNAMIC extent/protection")
    require(at(dynamic[3], dynamic[5]) == dynamic[2], "dynamic address mismatch")
    tags = {}
    needed = []
    null_seen = False
    for offset in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        tag, value = struct.unpack_from("<QQ", data, offset)
        if tag == 0:
            null_seen = True
            break
        if tag == 1:
            needed.append(value)
        else:
            require(tag not in tags, f"duplicate dynamic tag {tag}")
            tags[tag] = value
    require(null_seen, "missing DT_NULL")
    require(all(tag in tags for tag in (4, 5, 6, 7, 8, 9, 10, 11, 29)), "missing dynamic metadata")
    require(tags[9] == 24 and tags[11] == 24 and tags[8] % 24 == 0, "symbol/relocation widths")
    strings_start = at(tags[5], tags[10])
    strings = data[strings_start:strings_start + tags[10]]
    require(strings and strings[0] == 0 and strings[-1] == 0, "dynamic string terminators")

    def string(offset):
        require(offset < len(strings), "dynamic string offset")
        return strings[offset:strings.index(0, offset)].decode("utf-8")

    require(string(tags[29]) == "$ORIGIN", "runtime RUNPATH")
    dependencies = [string(offset) for offset in needed]
    require(len(set(dependencies)) == len(dependencies), "duplicate DT_NEEDED")
    hash_at = at(tags[4], 8)
    buckets, chains = struct.unpack_from("<II", data, hash_at)
    require(buckets == 1, "SysV hash bucket count")
    hash_size = (2 + buckets + chains) * 4
    at(tags[4], hash_size)
    symbols_at = at(tags[6], chains * 24)
    require(data[symbols_at:symbols_at + 24] == bytes(24), "dynamic null symbol")
    relocations_at = at(tags[7], tags[8])
    require(tags[8] // 24 == chains - 1, "import relocation count")
    slots = set()
    for i in range(chains - 1):
        name, info, other, section, value, size = struct.unpack_from("<IBBHQQ", data, symbols_at + (i + 1) * 24)
        require((info, other, section, value, size) == (0x12, 0, 0, 0, 0), "undefined function import")
        require(string(name), "empty import name")
        address, info, addend = struct.unpack_from("<QQq", data, relocations_at + i * 24)
        require(info == ((i + 1) << 32) | 6 and addend == 0, "GLOB_DAT import relocation")
        require(address not in slots, "overlapping import slots")
        slots.add(address)
        require(data[at(address, 8):at(address, 8) + 8] == bytes(8), "initial GOT slot")
        require(any(h[1] & 2 and h[3] <= address and address + 8 <= h[3] + h[6] for h in loads), "GOT slot protection")
    return dependencies


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("program", type=pathlib.Path)
    parser.add_argument("--expect-library", action="append", default=[])
    parser.add_argument("--expect-exit", type=int)
    args = parser.parse_args()
    dependencies = inspect(args.program)
    require(set(dependencies) == set(args.expect_library), f"dependencies: {dependencies}")
    if args.expect_exit is not None:
        result = subprocess.run([str(args.program.resolve())], check=False, timeout=60)
        require(result.returncode == args.expect_exit, f"exit code: {result.returncode}")
    print("dynamic ELF metadata checks passed")


if __name__ == "__main__":
    main()
