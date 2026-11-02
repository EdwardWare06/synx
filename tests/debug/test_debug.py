"""DWARF4, linker source maps, and native Linux GDB checks.

The binary reader here has no compiler imports or third-party dependencies.
Linux also uses readelf and GDB to check the emitted data with external tools.
"""
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
    env = dict(env)
    if os.environ.get('SYNX_DEBUG_ASAN'):
        env.update(LD_PRELOAD=os.environ['SYNX_DEBUG_ASAN'], ASAN_OPTIONS='detect_leaks=1:halt_on_error=1')
    p = subprocess.run(list(map(str, args)), env=env, capture_output=True, timeout=900)
    check(p.returncode == code, f'{args}: exit {p.returncode}\n{p.stdout!r}\n{p.stderr!r}')
    return p.stdout.decode('utf-8', 'replace') + p.stderr.decode('utf-8', 'replace')


def word(data, at, width=8):
    return int.from_bytes(data[at:at + width], 'little')


def cstring(data, at):
    end = data.index(0, at)
    return data[at:end].decode('utf-8'), end + 1


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def word(self, width):
        assert 0 <= self.at <= len(self.data) - width
        value = word(self.data, self.at, width)
        self.at += width
        return value

    def string(self):
        value, self.at = cstring(self.data, self.at)
        return value

    def leb(self, signed=False):
        value = shift = 0
        while True:
            b = self.word(1)
            value |= (b & 127) << shift
            shift += 7
            if not b & 128:
                return value - (1 << shift) if signed and b & 64 else value


class Elf:
    def __init__(self, data, relocate=False):
        check(data[:7] == b'\x7fELF\x02\x01\x01', 'ELF64 little endian')
        table, count, names = word(data, 40), word(data, 60, 2), word(data, 62, 2)
        self.headers = [struct.unpack_from('<IIQQQQIIQQ', data, table + i * 64) for i in range(count)]
        strings = self.headers[names]
        strings = data[strings[4]:strings[4] + strings[5]]
        self.sections, self.offsets = {}, {}
        self.names = [cstring(strings, h[0])[0] for h in self.headers]
        for name, h in zip(self.names, self.headers):
            self.sections[name] = bytearray(data[h[4]:h[4] + h[5]]) if h[1] != 8 else bytearray()
            self.offsets[name] = h[4]
        self.symbols = []
        if '.symtab' in self.sections:
            strings = self.sections['.strtab']
            s = self.sections['.symtab']
            for at in range(0, len(s), 24):
                name, info, other, section, value, size = struct.unpack_from('<IBBHQQ', s, at)
                self.symbols.append((cstring(strings, name)[0], section, value))
        if relocate:
            for name in ('.rela.debug_info', '.rela.debug_line'):
                r = self.sections[name]
                dst = self.sections[name.replace('.rela', '')]
                for at in range(0, len(r), 24):
                    offset, info, addend = struct.unpack_from('<QQq', r, at)
                    symbol, kind = info >> 32, info & 0xffffffff
                    _, section, value = self.symbols[symbol]
                    value += addend + (0x401000 if self.names[section] == '.text' else 0)
                    width = 8 if kind == 1 else 4
                    check(kind in (1, 10), 'DWARF relocation kind')
                    dst[offset:offset + width] = value.to_bytes(width, 'little')

    def relocations(self, name):
        data = self.sections[name]
        return [(offset, info & 0xffffffff, self.symbols[info >> 32][0], addend)
                for offset, info, addend in struct.iter_unpack('<QQq', data)]


def metadata(data):
    result = records.lines(data)
    check(True, 'checked source, type and variable metadata')
    return result


def dwarf(elf, meta, base):
    files, functions = meta
    r = Reader(elf.sections['.debug_abbrev'])
    abbreviations = {}
    while (code := r.leb()):
        tag, children, attrs = r.leb(), r.word(1), []
        while True:
            name, form = r.leb(), r.leb()
            if name == form == 0:
                break
            attrs.append((name, form))
        abbreviations[code] = tag, children, attrs
    r = Reader(elf.sections['.debug_info'])
    check(r.word(4) == len(r.data) - 4 and r.word(2) == 4 and r.word(4) == 0 and r.word(1) == 8, 'DWARF4 CU header')
    dies = []
    while r.at < len(r.data):
        code = r.leb()
        if code == 0:
            continue
        tag, children, attrs = abbreviations[code]
        values = {}
        for name, form in attrs:
            if form == 8:
                value = r.string()
            elif form == 0x0f:
                value = r.leb()
            elif form == 0x19:
                value = True
            else:
                value = r.word({1: 8, 5: 2, 6: 4, 0x0b: 1, 0x13: 4, 0x17: 4}[form])
            values[name] = value
        dies.append((tag, values))
    check(r.at == len(r.data), 'CU ends after child terminator')
    check(dies[0][0] == 0x11 and dies[0][1][0x25] == 'Synx Axon', 'CU producer')
    source_functions = [f for f in functions if f[4]]
    procedures = [die for die in dies if die[0] == 0x2e]
    check(len(procedures) == len(source_functions), 'all source functions have DIEs')
    check(dies[0][1][0x11] == base + min(f[2] for f in functions) and
          dies[0][1][0x12] == base + max(f[2] + f[3] for f in functions), 'CU address range')
    for (_, die), (symbol, name, address, size, rows) in zip(procedures, source_functions):
        check((die[3], die[0x11], die[0x12]) == (name, base + address, size), 'subprogram names and PC range')
        check((die[0x3a], die[0x3b]) == (rows[0][1] + 1, rows[0][2]), 'subprogram declaration')
    r = Reader(elf.sections['.debug_line'])
    check(r.word(4) == len(r.data) - 4 and r.word(2) == 4, 'DWARF4 line unit length/version')
    header_end = r.word(4) + 10
    check([r.word(1) for _ in range(6)] == [1, 1, 1, 251, 14, 13], 'line machine parameters')
    check([r.word(1) for _ in range(12)] == [0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1], 'standard opcode lengths')
    check(r.word(1) == 0, 'absolute source paths need no directory table')
    names = []
    while (name := r.string()):
        names.append(name)
        check([r.leb() for _ in range(3)] == [0, 0, 0], 'source file attributes')
    check(names == files and r.at == header_end, 'line file table and header length')
    sequences, rows = [], []
    pc, file, line, column, prologue = 0, 0, 1, 0, False
    while r.at < len(r.data):
        op = r.word(1)
        if op == 0:
            n = r.leb()
            ext = r.word(1)
            if ext == 2:
                check(n == 9, '64-bit line address')
                pc = r.word(8)
            else:
                check(ext == n == 1, 'end sequence opcode')
                sequences.append((rows, pc))
                pc, file, line, column, prologue, rows = 0, 0, 1, 0, False, []
        elif op == 1:
            rows.append((pc, file, line, column, prologue))
            prologue = False
        elif op == 2:
            pc += r.leb()
        elif op == 3:
            line += r.leb(True)
        elif op == 4:
            file = r.leb() - 1
        elif op == 5:
            column = r.leb()
        else:
            check(op in (6, 10), 'supported line opcode')
            if op == 10:
                prologue = True
    check(len(sequences) == len(source_functions), 'one terminated line sequence per function')
    for (actual, end), (_, _, address, size, rows) in zip(sequences, source_functions):
        expected = [(base + address + pc, file, line, col, i == 0) for i, (pc, file, line, col) in enumerate(rows)]
        expected.insert(0, (base + address, rows[0][1], rows[0][2], 0, False))
        check(actual == expected and end == base + address + size, 'exact source rows, prologue end, and function end')


def cross_host(synx, work, env):
    probe = work / ('debug-probe.exe' if os.name == 'nt' else 'debug-probe')
    run([synx, 'build', ROOT / 'selfhost/debug_probe.snx', '-o', probe], env)
    (work / 'calc.snx').write_text('pub int adjust(int x) {\n    int doubled = x * 2;\n    int result = doubled + 2;\n    return result;\n}\npub int sum(int stop) {\n    int total = 0;\n    for (n in 0..stop) {\n        total += n;\n    }\n    if (total == 0) { return stop; }\n    return total;\n}\n', encoding='utf-8')
    source = work / 'main.snx'
    source.write_text('import calc;\nint main() {\n    int answer = calc.adjust(20);\n    return answer - 42;\n}\n', encoding='utf-8')
    for mode in range(4):
        prefix = work / f'out{mode}'
        run([probe, source, prefix, mode], env)
        meta = metadata(Path(str(prefix) + '.meta').read_bytes())
        check(any(f.endswith('calc.snx') for f in meta[0]), 'imported source path retained')
        check(all(Path(p).is_absolute() for p in meta[0] if not p.startswith('<')), 'absolute source paths')
        obj = Elf(Path(str(prefix) + '.o').read_bytes(), relocate=True)
        plain = Elf(Path(str(prefix) + '.plain.o').read_bytes())
        check(obj.sections['.text'] == plain.sections['.text'] and obj.sections['.rodata'] == plain.sections['.rodata'], 'debug emission preserves machine code and constants')
        for section in ('.rela.text', '.rela.rodata'):
            check(obj.relocations(section) == plain.relocations(section), 'debug emission preserves native relocations')
        dwarf(obj, meta, 0x401000)
        dwarf(Elf(Path(str(prefix) + '.elf').read_bytes()), meta, 0x401000)
        dwarf(Elf(Path(str(prefix) + '.exe-elf').read_bytes()), meta, 0x401000)
        image = Path(str(prefix) + '.standalone').read_bytes()
        desc = image.index(b'SYNX-NATIVE-TEMPLATE-V1-X64-LOC\0')
        offset, size = word(image, desc + 40), word(image, desc + 48)
        payload = image[offset:offset + size]
        check(payload[-40:-32] == b'SNXJIT01', 'standalone debug footer')
        start, count, patch_start, patch_count = struct.unpack_from('<QQQQ', payload, len(payload) - 32)
        symbols = bytearray(payload[start:start + count])
        patches = list(struct.unpack_from(f'<{patch_count}Q', payload, patch_start))
        check(patches == sorted(set(patches)), 'load-base patches are unique and sorted')
        base = 0x7f3500000000
        for at in patches:
            symbols[at:at + 8] = (word(symbols, at) + base).to_bytes(8, 'little')
        dwarf(Elf(symbols), meta, base + 0x10000)
        original = Path(str(prefix) + '.o').read_bytes()
        custom = obj.offsets['.synx.debug']
        for at, replacement in [(custom, b'BADLINE1'), (custom + 8, b'\xff' * 4),
                                (custom + 12, b'\xff' * 4), (custom + 16, b'\xff' * 4)]:
            bad = bytearray(original)
            bad[at:at + len(replacement)] = replacement
            path = work / 'bad.o'
            path.write_bytes(bad)
            run([probe, path, prefix, 'read'], env, code=1)
        for corrupt in records.malformed_values(Path(str(prefix) + '.meta').read_bytes()):
            bad = bytearray(original)
            bad[custom:custom + len(corrupt)] = corrupt
            path = work / 'bad-values.o'
            path.write_bytes(bad)
            run([probe, path, prefix, 'read'], env, code=1)
    return source


def native(synx, source, work, env):
    if sys.platform != 'linux':
        print('Linux GDB execution checks run on Linux.')
        return
    check(shutil.which('gdb') and shutil.which('readelf'), 'Linux checks require gdb and readelf')
    for mode in range(4):
        flags = (['--no-regalloc'] if mode % 2 else []) + (['--release'] if mode >= 2 else [])
        for linker in ([], ['--shared-runtime'], ['--linker=system']):
            program = work / f'native-{mode}-{len(linker)}-{bool(linker and linker[0] == "--shared-runtime")}'
            run([synx, 'build', source, '-o', program, '--keep-obj', '-g', *flags, *linker], env)
            run([program], env)
            run(['readelf', '--debug-dump=info,line', str(program) + '.o'], env)
            script = work / 'gdb.txt'
            script.write_text('set pagination off\nset confirm off\nset backtrace past-main on\nset debuginfod enabled off\nset breakpoint pending on\nset disable-randomization off\n'
                              'break calc.snx:3\nrun\n'
                              'python s=gdb.newest_frame().find_sal(); assert s.line==3 and s.symtab.filename.endswith("calc.snx"); print("SYNX_BREAK_OK")\n'
                              'bt\nnext\n'
                              'python assert gdb.newest_frame().find_sal().line==4; print("SYNX_NEXT_OK")\n'
                              'finish\n'
                              'python s=gdb.newest_frame().find_sal(); assert s.symtab.filename.endswith("main.snx"); print("SYNX_FINISH_OK")\n'
                              'continue\n'
                              'delete breakpoints\nbreak main.snx:3\nrun\nstep\n'
                              'python s=gdb.newest_frame().find_sal(); assert s.symtab.filename.endswith("calc.snx") and s.line==2; print("SYNX_STEP_OK")\n'
                              'continue\n', encoding='utf-8')
            output = run(['gdb', '-q', '-nx', '-batch', '-x', script, program], env)
            check(all(s in output for s in ('SYNX_BREAK_OK', 'SYNX_NEXT_OK', 'SYNX_FINISH_OK', 'SYNX_STEP_OK')), 'GDB breakpoint, next, and stack return')
            check('adjust' in output and 'exited normally' in output, 'GDB function name and normal exit')
            check('synx_entry' in output, f'GDB startup bridge: mode={mode}, linker={linker}\n{output}')
            if not linker and mode == 0:
                image = bytearray(program.read_bytes())
                desc = image.index(b'SYNX-NATIVE-TEMPLATE-V1-X64-LOC\0')
                payload, size = word(image, desc + 40), word(image, desc + 48)
                footer = payload + size - 40
                blob, count, patches, npatch = struct.unpack_from('<QQQQ', image, footer + 8)
                first = word(image, payload + patches)
                mutations = [(footer + 8, 0), (footer + 16, 0xffffffffffffffff),
                             (footer + 24, 0), (footer + 32, 0xffffffffffffffff),
                             (payload + patches, count), (payload + patches + 8, first),
                             (payload + blob + first, 0xffffffffffffffff)]
                for at, value in mutations:
                    bad = bytearray(image)
                    bad[at:at + 8] = value.to_bytes(8, 'little')
                    broken = work / 'broken-debug'
                    broken.write_bytes(bad)
                    broken.chmod(0o700)
                    run([broken], env, code=126)
    for options, present in [([], True), (['--no-debug'], False), (['--release'], False), (['--release', '-g'], True), (['-g', '--release'], True)]:
        program = work / 'flags'
        run([synx, 'build', source, '-o', program, '--keep-obj', *options], env)
        elf = Elf(Path(str(program) + '.o').read_bytes())
        check(('.debug_line' in elf.sections) == present, 'default, release, and explicit debug flag precedence')
    waiting = work / 'wait.snx'
    waiting.write_text('import std.thread;\nmain() {\n    print("READY");\n    thread.sleep_ms(30000);\n}\n', encoding='utf-8')
    program = work / 'wait'
    run([synx, 'build', waiting, '-o', program], env)
    script = work / 'attach.gdb'
    script.write_text('set pagination off\nset confirm off\n'
                      'python s=gdb.decode_line("wait.snx:3")[1]; assert s and s[0].symtab.filename.endswith("wait.snx"); print("SYNX_ATTACH_OK")\n'
                      'bt\nkill\n', encoding='utf-8')
    output = run([sys.executable, ROOT / 'tests/debug/attach.py', program, script], env)
    check('SYNX_ATTACH_OK' in output and 'wait.snx' in output, 'GDB discovers retained symbols when attaching after startup')


def main():
    synx, runtime = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    env = {k: v for k, v in os.environ.items() if not k.startswith('SYNX_')}
    env.update(SYNX_LINKER="builtin", SYNX_STD_PATH=str(ROOT / 'std'), SYNX_RUNTIME_HOST=str(runtime / ('synx-host.exe' if os.name == 'nt' else 'synx-host')),
               SYNX_RUNTIME_LIB=str(runtime / ('synx_rt.lib' if os.name == 'nt' else 'libsynx_rt.a')),
               SYNX_RUNTIME_SHARED=str(runtime / {'win32': 'synx_rt.dll', 'darwin': 'libsynx_rt.dylib'}.get(sys.platform, 'libsynx_rt.so')))
    with tempfile.TemporaryDirectory(prefix='synx-debug-') as tmp:
        work = Path(tmp) / 'source space é'
        work.mkdir()
        source = cross_host(synx, work, env)
        if '--memory' not in sys.argv:
            native(synx, source, work, env)
    print(f'debug info: {checks} checks passed')


if __name__ == '__main__':
    main()
