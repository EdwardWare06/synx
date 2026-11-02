"""Independent MSF/PDB reader and native Windows source-debug acceptance."""
import hashlib
import binascii
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import records

ROOT = Path(__file__).resolve().parents[2]
checks = 0


def check(ok, message):
    global checks
    checks += 1
    if not ok:
        raise AssertionError(message)


def run(args, env, code=0):
    if os.environ.get('SYNX_PDB_ASAN'):
        env = dict(env, LD_PRELOAD=os.environ['SYNX_PDB_ASAN'], ASAN_OPTIONS='detect_leaks=1:halt_on_error=1')
    p = subprocess.run(list(map(str, args)), capture_output=True, env=env, timeout=900)
    check(p.returncode == code, f'{args}: {p.returncode}\n{p.stdout!r}\n{p.stderr!r}')
    return p.stdout.decode('utf8', 'replace') + p.stderr.decode('utf8', 'replace')


def word(data, at, width=4):
    check(0 <= at <= len(data) - width, 'field bounds')
    return int.from_bytes(data[at:at + width], 'little')


def cstr(data, at):
    return data[at:data.index(0, at)].decode('utf8')


class Pdb:
    def __init__(self, data):
        check(data[:32] == b'Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0', 'MSF signature')
        bs, fpm, count, size, reserved, block_map = struct.unpack_from('<6I', data, 32)
        check(bs == 4096 and fpm in (1, 2) and reserved == 0 and len(data) == count * bs, 'MSF superblock')
        page_count = (size + bs - 1) // bs
        dirs = struct.unpack_from(f'<{page_count}I', data, block_map * bs)
        directory = b''.join(data[p * bs:(p + 1) * bs] for p in dirs)[:size]
        n = word(directory, 0)
        sizes = struct.unpack_from(f'<{n}I', directory, 4)
        used = {0, block_map, *dirs}
        for i in range(count):
            if i % bs in (1, 2):
                used.add(i)
        self.streams = []
        at = 4 + n * 4
        for size in sizes:
            check(size != 0xffffffff and size <= len(data), 'stream size')
            np = (size + bs - 1) // bs
            pages = struct.unpack_from(f'<{np}I', directory, at)
            at += np * 4
            check(all(p < count and p not in used for p in pages) and len(set(pages)) == len(pages), 'unique stream pages')
            used.update(pages)
            self.streams.append(b''.join(data[p * bs:(p + 1) * bs] for p in pages)[:size])
        check(at == len(directory), 'complete stream directory')
        for i in range(count + 1):
            page = fpm + (i // 32768) * bs
            if page >= count:
                continue
            free = bool(data[page * bs + (i % 32768) // 8] & (1 << (i % 8)))
            check(free == (i not in used), 'FPM allocation and unused EOF bits')
        info = self.streams[1]
        check(word(info, 0) == 20000404 and word(info, 8) == 1, 'PDB version and age')
        self.guid = info[12:28]
        tpi = self.streams[2]
        hashes = self.streams[word(tpi, 20, 2)]
        count = word(tpi, 12) - 0x1000
        check(word(tpi, 8) == 0x1000 and word(tpi, 16) == len(tpi) - 56, 'TPI bounds')
        hash_bytes, index_at, index_bytes = word(tpi, 36), word(tpi, 40), word(tpi, 44)
        check(hash_bytes == count * 4 and index_at == hash_bytes and index_bytes == count * 8, 'TPI hash and index bounds')
        self.types = {}
        at = 56
        for i in range(count):
            size, kind = struct.unpack_from('<HH', tpi, at)
            end = at + size + 2
            check(size >= 2 and end <= len(tpi) and end % 4 == 0, 'type record extent')
            check(word(hashes, index_at + i * 8) == 0x1000 + i and word(hashes, index_at + i * 8 + 4) == at - 56, 'TPI index-to-offset map')
            expected = (binascii.crc32(tpi[at:end], 0xffffffff) ^ 0xffffffff) % 0x3ffff
            if kind in (0x1505, 0x1506, 0x1507) and not word(tpi, at + 6, 2) & 0x80:
                # Complete scoped UDTs hash the unique name, excluding NUL.
                name_at = at + (20 if kind == 0x1505 else 12 if kind == 0x1506 else 16)
                if kind != 0x1507:
                    numeric = word(tpi, name_at, 2)
                    name_at += 2 + ({0x8004: 4, 0x800a: 8}.get(numeric, 0) if numeric >= 0x8000 else 0)
                unique = cstr(tpi, tpi.index(0, name_at) + 1).encode('utf8')
                value = 0
                for p in range(0, len(unique) // 4 * 4, 4):
                    value ^= int.from_bytes(unique[p:p + 4], 'little')
                tail = unique[len(unique) // 4 * 4:]
                if len(tail) >= 2:
                    value ^= int.from_bytes(tail[:2], 'little')
                    tail = tail[2:]
                if tail:
                    value ^= tail[0]
                value |= 0x20202020
                value ^= value >> 11
                value ^= value >> 16
                expected = value % 0x3ffff
            check(word(hashes, i * 4) == expected, 'TPI record hash')
            self.types[0x1000 + i] = (kind, tpi[at:end])
            at = end
        check(at == len(tpi), 'complete type stream')
        dbi = self.streams[3]
        check(word(dbi, 0) == 0xffffffff and word(dbi, 4) == 19990903 and word(dbi, 8) == 1 and word(dbi, 58, 2) == 0x8664, 'DBI signature and machine')
        modules, contrib, section_map, sources, type_servers, mfc, optional, ec = struct.unpack_from('<8I', dbi, 24)
        check(len(dbi) == 64 + modules + contrib + section_map + sources + type_servers + optional + ec, 'DBI substream lengths')
        mod = dbi[64:64 + modules]
        index = word(mod, 34, 2)
        syms, c11, c13 = struct.unpack_from('<3I', mod, 36)
        check(c11 == 0 and len(self.streams[index]) == syms + c13 + 4, 'module source stream length')
        self.symbols = []
        module = self.streams[index]
        at = 4
        check(word(module, 0) == 4, 'CodeView signature')
        while at < syms:
            size, kind = struct.unpack_from('<HH', module, at)
            check(size >= 2 and at + size + 2 <= syms and (size + 2) % 4 == 0, 'CodeView symbol extent')
            if kind == 0x1110:
                end, length, offset, segment = word(module, at + 8), word(module, at + 16), word(module, at + 32), word(module, at + 36, 2)
                check(word(module, end + 2, 2) == 6, 'function end pointer')
                self.symbols.append((cstr(module, at + 39), segment, offset, length))
            at += size + 2
        self.lines, self.checksums = [], []
        at = syms
        subsections = []
        while at < syms + c13:
            kind, size = struct.unpack_from('<II', module, at)
            check(at + 8 + size <= syms + c13, 'C13 subsection extent')
            subsections.append((kind, module[at + 8:at + 8 + size]))
            at = (at + 8 + size + 3) & ~3
        names = self.streams[5]
        check(word(names, 0) == 0xeffeeffe and word(names, 4) == 1, 'global source string table')
        strings = names[12:12 + word(names, 8)]
        for kind, block in subsections:
            if kind == 0xf4:
                at = 0
                while at < len(block):
                    offset, size, alg = struct.unpack_from('<IBB', block, at)
                    check((alg, size) in ((0, 0), (3, 32)), 'source checksum kind')
                    self.checksums.append((at, cstr(strings, offset), block[at + 6:at + 6 + size]))
                    at = (at + 6 + size + 3) & ~3
            elif kind == 0xf2:
                offset, segment, flags, length = struct.unpack_from('<IHHI', block)
                check(flags == 1, 'column records present')
                at = 12
                while at < len(block):
                    file, count, size = struct.unpack_from('<III', block, at)
                    check(size == 12 + count * 12 and size <= len(block) - at, 'line block bounds')
                    for i in range(count):
                        pc, line = struct.unpack_from('<II', block, at + 12 + i * 8)
                        check(pc < length, 'line inside function')
                        self.lines.append((segment, offset + pc, file, line & 0xffffff, bool(line & 0x80000000)))
                    at += size
        optional_at = 64 + modules + contrib + section_map + sources + type_servers + ec
        sec_stream = word(dbi, optional_at + 10, 2)
        self.sections = self.streams[sec_stream]
        check(len(self.sections) % 40 == 0 and bool(self.symbols), 'section headers and functions')


class Pe:
    def __init__(self, data):
        self.data = data
        self.pe = word(data, 60)
        self.opt = self.pe + 24
        count = word(data, self.pe + 6, 2)
        self.sections = data[self.pe + 264:self.pe + 264 + count * 40]
        self.base = word(data, self.opt + 24, 8)

    def offset(self, rva, size):
        for at in range(0, len(self.sections), 40):
            start, raw_size, raw = struct.unpack_from('<III', self.sections, at + 12)
            if start <= rva and rva + size <= start + raw_size:
                return raw + rva - start
        raise AssertionError('RVA absent from section raw bytes')

    def debug(self):
        rva, size = struct.unpack_from('<II', self.data, self.opt + 160)
        check(size and size % 28 == 0, 'PE debug directory entries')
        at = self.offset(rva, size)
        at = next(at + i for i in range(0, size, 28) if word(self.data, at + i + 12) == 2)
        kind, length, address, raw = struct.unpack_from('<4I', self.data, at + 12)
        check(kind == 2 and raw == self.offset(address, length), 'debug directory file and virtual addresses')
        record = self.data[raw:raw + length]
        check(record[:4] == b'RSDS' and word(record, 20) == 1, 'RSDS record')
        return record[4:20], cstr(record, 24)

    def unwind(self):
        rva, size = struct.unpack_from('<II', self.data, self.opt + 136)
        check(size and size % 12 == 0, 'PE exception directory')
        at = self.offset(rva, size)
        previous = 0
        large = False
        native = next((word(self.sections, at + 12) for at in range(0, len(self.sections), 40) if self.sections[at:at + 8] == b'.snxtext'), None)
        for pos in range(at, at + size, 12):
            begin, end, info = struct.unpack_from('<III', self.data, pos)
            check(previous <= begin < end, 'sorted disjoint runtime functions')
            previous = end
            u = self.offset(info, 4)
            check(self.data[u] & 7 in (1, 2), 'x64 unwind version, including prelinked MSVC records')
            if native is not None and begin >= native and self.data[u] == 1:
                slot = 0
                while slot < self.data[u + 2]:
                    op = self.data[u + 5 + slot * 2]
                    kind, value = op & 15, op >> 4
                    if kind == 1 and value == 1:
                        large = True
                    slot += 1 + (2 if kind in (5, 9) or (kind == 1 and value == 1) else 1 if kind in (4, 8) or kind == 1 else 0)
        return large


def main():
    synx, runtime, harness_dir = map(lambda x: Path(x).resolve(), sys.argv[1:4])
    env = {k: v for k, v in os.environ.items() if not k.startswith('SYNX_')}
    env.update(SYNX_LINKER='builtin', SYNX_STD_PATH=str(ROOT / 'std'), SYNX_RUNTIME_HOST=str(runtime / ('synx-host.exe' if os.name == 'nt' else 'synx-host')),
               SYNX_RUNTIME_SHARED=str(runtime / {'win32': 'synx_rt.dll', 'darwin': 'libsynx_rt.dylib'}.get(sys.platform, 'libsynx_rt.so')),
               SYNX_RUNTIME_LIB=str(runtime / ('synx_rt.lib' if os.name == 'nt' else 'libsynx_rt.a')))
    with tempfile.TemporaryDirectory(prefix='synx-pdb-') as tmp:
        work = Path(tmp) / 'source space café'
        work.mkdir()
        backend_probe = work / ('backend-probe.exe' if os.name == 'nt' else 'backend-probe')
        run([synx, 'build', ROOT / 'selfhost/debug_probe.snx', '-o', backend_probe], env)
        source = work / 'main.snx'
        source.write_text('import calc;\nint main() {\n    int value = calc.adjust(20);\n    return value - 42;\n}\n', encoding='utf8')
        calc = work / 'calc.snx'
        calc.write_text('pub int adjust(int x) {\n    int doubled = x * 2;\n    int result = doubled + 2;\n    return result;\n}\n', encoding='utf8')
        for mode in range(4):
            flags = (['--no-regalloc'] if mode % 2 else []) + (['--release'] if mode >= 2 else [])
            probe = work / ('probe.exe' if os.name == 'nt' else 'probe')
            run([synx, 'build', ROOT / 'selfhost/pdb_probe.snx', '-o', probe, *flags], env)
            output = work / 'probe.pdb'
            run([probe, output], env)
            parsed = Pdb(output.read_bytes())
            check(parsed.symbols == [('example', 1, 0, 32)], 'known procedure record')
            check([(r[1], r[3], r[4]) for r in parsed.lines] == [(0, 3, False), (4, 3, True), (16, 4, True)], 'known statement lines')
            if mode == 0:
                run([probe, output, 'large'], env)
                large = output.read_bytes()
                check(len(large) > 16 * 1024 * 1024, 'MSF crosses reserved FPM interval')
                Pdb(large)
            prefix = work / f'cross-{mode}'
            run([backend_probe, source, prefix, f'win{mode}'], env)
            cross_pdb = Pdb(Path(str(prefix) + '.pdb').read_bytes())
            cross_pe = Pe(Path(str(prefix) + '.exe').read_bytes())
            check(cross_pe.debug()[0] == cross_pdb.guid, 'cross-host PE/PDB identities')
            cross_pe.unwind()
            obj = Path(str(prefix) + '.obj').read_bytes()
            sections = {obj[at:at + 8].rstrip(b'\0'): at for at in range(20, 20 + word(obj, 2, 2) * 40, 40)}
            check(all(s in sections for s in (b'.debug$S', b'.debug$T', b'.snxdbg', b'.pdata', b'.xdata')), 'COFF CodeView and unwind sections')
            custom = word(obj, sections[b'.snxdbg'] + 20)
            for at, value in ((custom, b'BADLINE2'), (custom + 8, b'\xff' * 4), (custom + 12, b'\xff' * 4), (custom + 16, b'\xff' * 4)):
                bad = bytearray(obj)
                bad[at:at + len(value)] = value
                corrupt = work / 'corrupt.obj'
                corrupt.write_bytes(bad)
                run([backend_probe, corrupt, prefix, 'read-coff'], env, code=1)
            for corrupt_values in records.malformed_values(Path(str(prefix) + '.meta').read_bytes()):
                bad = bytearray(obj)
                bad[custom:custom + len(corrupt_values)] = corrupt_values
                corrupt = work / 'corrupt-values.obj'
                corrupt.write_bytes(bad)
                run([backend_probe, corrupt, prefix, 'read-coff'], env, code=1)
            if os.name != 'nt':
                continue
            for variant, linker in enumerate(([], ['--shared-runtime'], ['--linker=system'])):
                program = work / f'program-{mode}-{variant}.exe'
                run([synx, 'build', source, '-o', program, '-g', '--keep-obj', *flags, *linker], env)
                run([program], env)
                pdb_path = program.with_suffix('.pdb')
                pe = Pe(program.read_bytes())
                guid, name = pe.debug()
                # System linkers emit additional native runtime module records.
                if variant != 2:
                    info = Pdb(pdb_path.read_bytes())
                    check(info.guid == guid and name == pdb_path.name and info.sections == pe.sections, 'matching PE/PDB identity and sections')
                    for _, file, checksum in info.checksums:
                        if file in (str(source), str(calc)) or file.replace('\\', '/') in (source.as_posix(), calc.as_posix()):
                            check(checksum == hashlib.sha256(Path(file).read_bytes()).digest(), 'checksum matches compiled source bytes')
                    pe.unwind()
                result = run([harness_dir / 'test_windows_debug.exe', program], env)
                check('SYNX_WINDOWS_DEBUG_OK' in result, 'Windows source breakpoint, step, caller return and native stack walk')
                if variant == 0:
                    # Debug companion is optional for native execution.
                    saved = pdb_path.read_bytes()
                    pdb_path.unlink()
                    run([program], env)
                    pdb_path.write_bytes(saved)
                    first = program.read_bytes(), saved
                    run([synx, 'build', source, '-o', program, '-g', *flags], env)
                    check(first == (program.read_bytes(), pdb_path.read_bytes()), 'deterministic executable and PDB bytes')
        if os.name == 'nt':
            # A frame above 512 KiB exercises the 32-bit ALLOC_LARGE form,
            # compact guard-page probes, and actual per-boundary OS unwinding.
            calc.write_text('pub int adjust(int x) {\n    int[70000] page = [x; 70000]; int doubled = page[0] * 2;\n    int result = doubled + 2;\n    return result;\n}\n', encoding='utf8')
            program = work / 'large-frame.exe'
            run([synx, 'build', source, '-o', program, '-g'], env)
            check(Pe(program.read_bytes()).unwind(), 'frame uses 32-bit ALLOC_LARGE record')
            result = run([harness_dir / 'test_windows_debug.exe', program], env)
            check('SYNX_WINDOWS_DEBUG_OK' in result and 'instruction boundaries' in result, 'large frame prologue/body/epilogue OS unwinding')
            calc.write_text('import std.io; pub int adjust(int x) {\n    int doubled = x * 2;\n    int result = doubled + 2;\n    return result;\n}\npub void ready() { print("READY"); var input = io.read_line(); }\n', encoding='utf8')
            source.write_text('import calc;\nint main() {\n    calc.ready(); int value = calc.adjust(20);\n    return value - 42;\n}\n', encoding='utf8')
            program = work / 'attach.exe'
            run([synx, 'build', source, '-o', program], env)
            check('SYNX_WINDOWS_ATTACH_OK' in run([harness_dir / 'test_windows_debug.exe', program, '--attach'], env), 'symbols, breakpoints and stack walk after startup attachment')
            calc.write_text('pub int adjust(int x) {\n    int doubled = x * 2;\n    int result = doubled + 2;\n    return result;\n}\n', encoding='utf8')
            source.write_text('import calc;\nint main() {\n    int value = calc.adjust(20);\n    return value - 42;\n}\n', encoding='utf8')
            for flags, expected in (([], True), (['--no-debug'], False), (['--release'], False), (['--release', '-g'], True), (['-g', '--release'], True)):
                program = work / 'flags.exe'
                program.with_suffix('.pdb').unlink(missing_ok=True)
                run([synx, 'build', source, '-o', program, *flags], env)
                pe = Pe(program.read_bytes())
                # Prelinked host may carry its own native CodeView directory.
                check(program.with_suffix('.pdb').exists() == expected, 'debug default and explicit flag precedence')
    print(f'PDB debug info: {checks} checks passed')


if __name__ == '__main__':
    main()
