#!/usr/bin/env python3
"""Validate Mach-O dyld fixups and every CodeDirectory page hash.

Usage: python tests/linker/macho_dynamic_test.py executable [executable ...]
This is structural cross-host validation. It does not execute macOS programs.
"""
import argparse
import hashlib
from pathlib import Path
import struct


def u32(data, at):
    return struct.unpack_from("<I", data, at)[0]


def cstr(data, at, end):
    stop = data.index(0, at, end)
    return data[at:stop]


def uleb(data, at, end):
    value = 0
    shift = 0
    while at < end:
        ch = data[at]
        at += 1
        value |= (ch & 127) << shift
        if not ch & 128:
            return value, at
        shift += 7
        assert shift < 64
    raise AssertionError("truncated ULEB128")


def verify(data):
    magic, cpu, subtype, kind, ncmds, sizeofcmds, flags, reserved = struct.unpack_from("<8I", data)
    assert (magic, cpu, subtype, kind, reserved) == (0xFEEDFACF, 0x1000007, 3, 2, 0)
    assert flags & 0x200085 == 0x200085
    endcmds = 32 + sizeofcmds
    assert endcmds <= 4096
    at = 32
    segments = []
    commands = {}
    libraries = []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, at)
        assert size >= 8 and size % 8 == 0 and at + size <= endcmds
        if cmd == 0x19:
            name = data[at + 8:at + 24].rstrip(b"\0")
            va, vmsize, fileoff, filesize, maxprot, prot, nsects, segflags = struct.unpack_from("<QQQQIIII", data, at + 24)
            assert size == 72 + nsects * 80
            assert va % 4096 == vmsize % 4096 == fileoff % 4096 == 0
            assert fileoff + filesize <= len(data) and filesize <= vmsize
            assert prot & maxprot == prot and not (prot & 2 and prot & 4)
            segments.append((name, va, vmsize, fileoff, filesize, maxprot, prot))
            for i in range(nsects):
                sect = at + 72 + 80 * i
                assert data[sect + 16:sect + 32].rstrip(b"\0") == name
                address, extent, offset, align = struct.unpack_from("<QQII", data, sect + 32)
                attributes = u32(data, sect + 64)
                assert va <= address <= address + extent <= va + vmsize
                assert address % (1 << align) == 0
                if attributes & 255 != 1:
                    assert fileoff <= offset <= offset + extent <= fileoff + filesize
        elif cmd == 0xC:
            libraries.append(cstr(data, at + u32(data, at + 8), at + size))
        else:
            assert cmd not in commands
            commands[cmd] = (at, size)
        at += size
    assert at == endcmds
    assert [s[0] for s in segments] == [b"__PAGEZERO", b"__TEXT", b"__DATA", b"__LINKEDIT"]
    assert segments[0][1:3] == (0, 0x100000000)
    assert segments[1][1] == 0x100000000 and segments[1][6] == 5
    assert segments[2][6] == 3 and segments[3][6] == 1
    assert b"/usr/lib/libSystem.B.dylib" in libraries
    pos, _ = commands[0xE]
    assert cstr(data, pos + u32(data, pos + 8), pos + commands[0xE][1]) == b"/usr/lib/dyld"
    pos, _ = commands[0x8000001C]
    assert cstr(data, pos + u32(data, pos + 8), pos + commands[0x8000001C][1]) == b"@loader_path"
    pos, _ = commands[0x80000028]
    entryoff, stack = struct.unpack_from("<QQ", data, pos + 8)
    assert 4096 <= entryoff < segments[1][4] and stack == 8388608
    info, _ = commands[0x80000022]
    rebase_at, rebase_size, bind_at, bind_size, weak_at, weak_size, lazy_at, lazy_size, export_at, export_size = struct.unpack_from("<10I", data, info + 8)
    assert rebase_at % 8 == bind_at % 8 == 0
    assert (weak_at, weak_size, lazy_at, lazy_size, export_at, export_size) == (0,) * 6
    def pointer_fileoff(segment, offset):
        assert segment == 2 and offset + 8 <= segments[segment][4]
        return segments[segment][3] + offset
    rebase_sites = []
    at, end = rebase_at, rebase_at + rebase_size
    while at < end:
        op = data[at]
        at += 1
        if op == 0:
            assert at == end
            break
        assert op == 0x11 and data[at] == 0x22
        at += 1
        offset, at = uleb(data, at, end)
        assert data[at] == 0x51
        at += 1
        site = pointer_fileoff(2, offset)
        assert site not in rebase_sites
        rebase_sites.append(site)
        pointer = struct.unpack_from("<Q", data, site)[0]
        assert segments[1][1] <= pointer < segments[3][1]
    else:
        raise AssertionError("missing rebase DONE")
    binds = []
    at, end = bind_at, bind_at + bind_size
    while at < end:
        op = data[at]
        at += 1
        if op == 0:
            assert at == end
            break
        if op == 0x20:
            ordinal, at = uleb(data, at, end)
        else:
            assert op & 0xF0 == 0x10
            ordinal = op & 15
        assert 1 <= ordinal <= len(libraries) and data[at] == 0x40
        at += 1
        name = cstr(data, at, end)
        assert name
        at += len(name) + 1
        assert data[at:at + 2] == b"\x51\x72"
        at += 2
        offset, at = uleb(data, at, end)
        assert data[at] == 0x90
        at += 1
        site = pointer_fileoff(2, offset)
        assert site not in rebase_sites and data[site:site + 8] == b"\0" * 8
        assert site not in [b[2] for b in binds]
        binds.append((libraries[ordinal - 1], name, site))
    else:
        raise AssertionError("missing bind DONE")
    sigcmd, _ = commands[0x1D]
    sigoff, sigsize = struct.unpack_from("<II", data, sigcmd + 8)
    assert sigoff % 16 == 0 and sigoff + sigsize == len(data)
    assert segments[3][3] <= rebase_at < bind_at < sigoff
    signature = data[sigoff:]
    assert struct.unpack_from(">5I", signature) == (0xFADE0CC0, sigsize, 1, 0, 20)
    cd = signature[20:]
    magic, size, version, cdflags, hashes, ident, special, count, limit = struct.unpack_from(">9I", cd)
    assert (magic, size, version, cdflags, special, limit) == (0xFADE0C02, len(cd), 0x20400, 0x20002, 0, sigoff)
    assert cd[36:40] == bytes([32, 2, 0, 12])
    assert cstr(cd, ident, hashes) == b"synx"
    assert count == (sigoff + 4095) // 4096 and hashes + count * 32 == len(cd)
    assert struct.unpack_from(">QQQ", cd, 64) == (0, segments[1][4], 1)
    for i in range(count):
        assert cd[hashes + 32 * i:hashes + 32 * (i + 1)] == hashlib.sha256(data[i * 4096:min((i + 1) * 4096, sigoff)]).digest()
    return {"imports": len(binds), "rebases": len(rebase_sites), "pages": count, "sha256": hashlib.sha256(data).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images", nargs="+", type=Path)
    args = parser.parse_args()
    for image in args.images:
        print(image, verify(image.read_bytes()))


if __name__ == "__main__":
    main()
