"""Private run-cache and single-file executable checks on the current host."""
import concurrent.futures
import ctypes
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
EXE = '.exe' if os.name == 'nt' else ''
RUNTIME = {'win32': 'synx_rt.dll', 'darwin': 'libsynx_rt.dylib'}.get(sys.platform, 'libsynx_rt.so')
MARKER = b'SYNX-NATIVE-TEMPLATE-V1-X64-LOC\0'
MODES = [[], ['--no-regalloc'], ['--release'], ['--no-regalloc', '--release']]
checks = 0

def check(value, label, detail=''):
    global checks
    checks += 1
    if not value:
        raise AssertionError(label + ('\n' + detail if detail else ''))

def macho_template(signature=True):
    # Minimal x86-64 template with reserved header padding and LINKEDIT records.
    data = bytearray(8192)
    commands = []
    def segment(name, vmaddr, fileoff, size):
        return struct.pack('<II16sQQQQIIII', 0x19, 72, name, vmaddr, size, fileoff, size, 7, 5, 0, 0)
    commands.append(segment(b'__TEXT', 0x100000000, 0, 4096))
    commands.append(segment(b'__LINKEDIT', 0x100001000, 4096, 4096))
    if signature:
        commands.append(struct.pack('<IIII', 0x1d, 16, 8192, 32))
    command_bytes = b''.join(commands)
    struct.pack_into('<IIIIIIII', data, 0, 0xfeedfacf, 0x1000007, 3, 2, len(commands), len(command_bytes), 0, 0)
    data[32:32 + len(command_bytes)] = command_bytes
    if signature:
        data.extend(b'X' * 32)
    return data

def verify_macho(data):
    cursor = 32
    signature = None
    linkedit = None
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        cmd, size = struct.unpack_from('<II', data, cursor)
        if cmd == 0x1d:
            signature = struct.unpack_from('<II', data, cursor + 8)
        if cmd == 0x19 and data[cursor + 8:cursor + 24].rstrip(b'\0') == b'__LINKEDIT':
            linkedit = struct.unpack_from('<QQ', data, cursor + 40)
        cursor += size
    check(signature is not None and linkedit is not None, 'Mach-O signature and LINKEDIT commands')
    offset, size = signature
    check(offset + size == len(data) and sum(linkedit) == len(data), 'Mach-O signature and LINKEDIT extents')
    magic, length, count, kind, relative = struct.unpack_from('>IIIII', data, offset)
    check((magic, length, count, kind, relative) == (0xfade0cc0, size, 1, 0, 20), 'signature superblob')
    cd = offset + relative
    magic, length, version, flags, hash_offset, ident_offset, special, pages, limit = struct.unpack_from('>IIIIIIIII', data, cd)
    check(magic == 0xfade0c02 and length == size - 20 and version == 0x20400 and limit == offset, 'CodeDirectory header')
    check(data[cd + ident_offset:cd + ident_offset + 5] == b'synx\0', 'CodeDirectory identifier')
    for page in range(pages):
        digest = hashlib.sha256(data[page * 4096:min((page + 1) * 4096, offset)]).digest()
        check(data[cd + hash_offset + page * 32:cd + hash_offset + (page + 1) * 32] == digest, f'CodeDirectory page {page}')

def main():
    synx, runtime = map(lambda p: Path(p).resolve(), sys.argv[1:])
    work = Path(tempfile.mkdtemp(prefix='synx-workflow-'))
    env = {k: v for k, v in os.environ.items() if not k.startswith('SYNX_')}
    env.update(SYNX_STD_PATH=str(ROOT / 'std'), SYNX_RUNTIME_HOST=str(runtime / ('synx-host' + EXE)),
               SYNX_RUNTIME_SHARED=str(runtime / RUNTIME), SYNX_CACHE_DIR=str(work / 'private cache'))
    src = work / 'source café λ'
    src.mkdir()
    entry = src / 'main.snx'
    helper = src / 'helper.snx'
    helper.write_text('pub int value() { return 7; }\n', encoding='utf8')
    entry.write_text('import helper; import std.io; import std.env; import std.fs; '
                     'int main() { var args = env.args(); for (i in 1..args.len()) { io.print(args.get(i).unwrap()); } '
                     'match fs.current_dir() { Ok(value) => io.print(value), Err(_) => return 90 } return helper.value(); }\n', encoding='utf8')
    def run(args, executable=synx):
        r = subprocess.run([str(executable), *map(str, args)], cwd=src, env=env, capture_output=True, timeout=900)
        return r.returncode, r.stdout.decode('utf8', 'replace').replace('\r\n', '\n'), r.stderr.decode('utf8', 'replace').replace('\r\n', '\n')
    def compiled(flags=(), extra=()):
        result = run(['run', entry, '--verbose', *flags, *extra, '--', 'two words', '', 'café λ', '&|<>^%"$()'])
        check(result[0] == 7 and result[1].splitlines()[:-1] == ['two words', '', 'café λ', '&|<>^%"$()'], 'arguments and exit status', str(result))
        check(Path(result[1].splitlines()[-1]) == src, 'working directory inherited')
        check(set(p.name for p in src.iterdir()) == {'main.snx', 'helper.snx'}, 'source folder contains only sources')
        return result
    try:
        # Compare native SHA-256 against an independent implementation at block boundaries.
        lib = None
        if sys.platform == 'darwin':
            # Apple Silicon runners can have arm64 Python and x86-64 Synx under Rosetta.
            hash_probe = work / ('hash-probe' + EXE)
            result = run(['build', ROOT / 'tests/workflow/hash_probe.snx', '-o', hash_probe])
            check(result[0] == 0, 'native hash probe builds', str(result))
        else:
            lib = ctypes.CDLL(str(runtime / RUNTIME))
            h = lib.synx_rt_cache_hash
            h.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p]
        for size in [0, 1, 55, 56, 63, 64, 65, 119, 120, 4096, 65539]:
            data = bytes((i * 17 + 3) % 256 for i in range(size))
            if lib is None:
                source, destination = work / 'hash-input', work / 'hash-output'
                source.write_bytes(data)
                result = run([source, destination], hash_probe)
                check(result[0] == 0, 'native hash probe runs', str(result))
                digest = destination.read_bytes()
            else:
                output = (ctypes.c_ubyte * 32)()
                h(data, len(data), output)
                digest = bytes(output)
            check(digest == hashlib.sha256(data).digest(), f'SHA256 length {size}')
        default_entry = None
        for flags in MODES:
            check('run cache: miss' in compiled(flags)[2], f'first run misses {flags}')
            check('run cache: hit' in compiled(flags)[2], f'second run hits {flags}')
            if not flags:
                default_entry = next(Path(env['SYNX_CACHE_DIR']).glob('*/entry'))
        cache = Path(env['SYNX_CACHE_DIR'])
        manifests = sorted(cache.glob('*/entry'))
        check(len(manifests) == 4, 'four mode entries')
        manifest = default_entry
        name, digest, _ = manifest.read_text().split('\n')
        cached = manifest.parent / name
        check(hashlib.sha256(cached.read_bytes()).hexdigest() == digest, 'manifest records independent executable digest')
        cached.write_bytes(b'corrupt')
        check('run cache: miss' in compiled()[2], 'corrupt output rebuilds')
        # Source changes must invalidate even when modification time is retained.
        stat = helper.stat()
        helper.write_text('pub int value() { return 8; }\n', encoding='utf8')
        os.utime(helper, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        changed = run(['run', entry, '--verbose'])
        check(changed[0] == 8 and 'run cache: miss' in changed[2], 'dependency contents invalidate cache', str(changed))
        helper.write_text('pub int value() { return 7; }\n', encoding='utf8')
        before = {p: p.read_bytes() for p in cache.glob('*/entry')}
        generations = set(cache.glob('*/program-*'))
        check('run cache: miss' in compiled(extra=['--no-cache'])[2], '--no-cache compiles')
        check(before == {p: p.read_bytes() for p in cache.glob('*/entry')}, '--no-cache leaves entries unchanged')
        check(generations == set(cache.glob('*/program-*')), '--no-cache removes temporary executable')
        # One entry lock serializes publication. All callers can then execute.
        helper.write_text('pub int value() { return 7; } // concurrent\n', encoding='utf8')
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: run(['run', entry, '--verbose']), range(4)))
        check(all(r[0] == 7 for r in results), 'concurrent runs succeed', str(results))
        check(sum('run cache: miss' in r[2] for r in results) == 1, 'concurrent entry builds once', str(results))
        for mode, flags in enumerate(MODES):
            standalone = work / ('single-' + str(mode) + EXE)
            result = run(['build', entry, '-o', standalone, *flags])
            check(result[0] == 0, f'standalone builds {mode}', str(result))
            isolated = work / ('isolated-' + str(mode))
            isolated.mkdir()
            moved = isolated / ('renamed café' + EXE)
            shutil.copy2(standalone, moved)
            result = run(['hello'], moved)
            check(result[0] == 7 and result[1].splitlines()[0] == 'hello', f'one moved executable runs {mode}', str(result))
            check(list(isolated.iterdir()) == [moved], 'standalone needs no sidecar')
            if sys.platform == 'darwin':
                r = subprocess.run(['codesign', '--verify', '--strict', str(moved)], capture_output=True)
                check(r.returncode == 0, 'macOS signature verifies', r.stderr.decode())
            data = bytearray(moved.read_bytes())
            desc = data.index(MARKER)
            payload, size = struct.unpack_from('<QQ', data, desc + 40)
            check(data[payload:payload + 8] == b'SNXPAY01' and payload + size <= len(data), 'native payload locator')
            # Signed macOS files reject changed bytes before startup. Other hosts reach the checked loader.
            if sys.platform != 'darwin' and mode == 0:
                for label, offset, value in [('magic', payload, b'BADIMAGE'), ('ABI', payload + 8, b'\0' * 32),
                                              ('segment count', payload + 64, struct.pack('<Q', 9)),
                                              ('text protection', payload + 88 + 32, struct.pack('<Q', 7))]:
                    damaged = bytearray(data)
                    damaged[offset:offset + len(value)] = value
                    bad = isolated / ('invalid-' + label.replace(' ', '-') + EXE)
                    bad.write_bytes(damaged)
                    bad.chmod(0o700)
                    result = run([], bad)
                    check(result[0] == 126 and 'invalid standalone image' in result[2], f'loader rejects {label}', str(result))
        if sys.platform.startswith('linux'):
            # Execute an open inode after unlink, so /proc/self/exe has a deleted name.
            deleted = work / 'unlinked-program'
            shutil.copy2(standalone, deleted)
            fd = os.open(deleted, os.O_RDONLY)
            try:
                deleted.unlink()
                result = subprocess.run([f'/proc/self/fd/{fd}', 'unlinked'], pass_fds=(fd,), env=env, cwd=src, capture_output=True, timeout=30)
                check(result.returncode == 7 and result.stdout.startswith(b'unlinked\n'), 'Linux startup reads running inode after unlink', repr(result))
            finally:
                os.close(fd)
        # A changed std module invalidates a hit even at the same path and mtime.
        private_std = work / 'std'
        shutil.copytree(ROOT / 'std', private_std)
        env['SYNX_STD_PATH'] = str(private_std)
        check('run cache: miss' in compiled()[2], 'new std path changes key')
        check('run cache: hit' in compiled()[2], 'unchanged private std reuses output')
        io = private_std / 'io.snx'
        times = io.stat()
        io.write_bytes(io.read_bytes() + b'\n// workflow dependency change\n')
        os.utime(io, ns=(times.st_atime_ns, times.st_mtime_ns))
        check('run cache: miss' in compiled()[2], 'std contents change key with unchanged mtime')
        env['SYNX_STD_PATH'] = str(ROOT / 'std')
        # Program arguments do not affect compilation or cache identity.
        result = run(['run', entry, '--verbose', '--', 'different'])
        check(result[0] == 7 and result[1].startswith('different\n') and 'run cache: hit' in result[2], 'different child arguments reuse cache')
        # Give the probe the same module root as the compiler without editing selfhost/.
        probe_root = work / 'compiler-probe'
        shutil.copytree(ROOT / 'selfhost', probe_root)
        shutil.copy2(ROOT / 'tests/workflow/macho_probe.snx', probe_root / 'workflow_probe.snx')
        # The signer is verified on every host, including add-command and replace-command paths.
        for mode, flags in enumerate(MODES):
            probe = work / ('macho-probe-' + str(mode) + EXE)
            result = run(['build', probe_root / 'workflow_probe.snx', '-o', probe, *flags])
            check(result[0] == 0, 'Mach-O signer probe builds', str(result))
            for signed in [False, True]:
                template = work / 'synthetic-macho'
                template.write_bytes(macho_template(signed))
                output = work / 'synthetic-signed'
                result = run([template, output], probe)
                check(result[0] == 0, 'Mach-O signer probe runs', str(result))
                verify_macho(output.read_bytes())
        host = bytearray((runtime / ('synx-host' + EXE)).read_bytes())
        desc = host.index(MARKER)
        mutations = [('duplicate locator', len(host), MARKER + bytes(96)), ('ABI catalogue', desc + 56, bytes(32)),
                     ('wrong OS', desc + 36, struct.pack('<I', 99)), ('already patched', desc + 40, struct.pack('<Q', 1))]
        if os.name == 'nt':
            pe = struct.unpack_from('<I', host, 60)[0]
            mutations.append(('Authenticode', pe + 24 + 144, struct.pack('<II', len(host), 8)))
        for label, offset, value in mutations:
            data = bytearray(host)
            data[offset:offset + len(value)] = value
            template = work / 'bad-template'
            template.write_bytes(data)
            env['SYNX_RUNTIME_HOST'] = str(template)
            result = run(['build', entry, '-o', work / ('rejected' + EXE)])
            check(result[0] == 1 and 'built-in linker:' in result[2], 'reject template ' + label, str(result))
        env['SYNX_RUNTIME_HOST'] = str(runtime / ('synx-host' + EXE))
        # Legacy shared mode verifies its copied runtime before reuse.
        for attempt in range(2):
            result = compiled(extra=['--shared-runtime'])
            check(('run cache: miss' if attempt == 0 else 'run cache: hit') in result[2], 'shared cache reuse')
        capsule = next(Path(env['SYNX_CACHE_DIR']).glob('*/' + RUNTIME))
        capsule.unlink()
        check('run cache: miss' in compiled(extra=['--shared-runtime'])[2], 'missing copied capsule rebuilds')
        if os.name == 'nt':
            insecure = work / 'insecure-cache'
            prepare = lib.synx_rt_cache_prepare
            prepare.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
            encoded = str(insecure).encode('utf8')
            check(prepare(encoded, len(encoded)) == 0, 'native cache creates protected Windows directory')
            grant = subprocess.run(['icacls', str(insecure), '/grant', '*S-1-1-0:(OI)(CI)F'], capture_output=True)
            check(grant.returncode == 0, 'test grants Everyone access to isolated test directory', repr(grant))
            env['SYNX_CACHE_DIR'] = str(insecure)
            result = run(['run', entry])
            check(result[0] == 1 and 'cannot create private run cache' in result[2], 'Windows rejects cache ACL with Everyone access', str(result))
        if os.name != 'nt':
            import stat
            permissions = work / 'existing-private'
            permissions.mkdir(mode=0o750)
            env['SYNX_CACHE_DIR'] = str(permissions)
            result = run(['run', entry])
            check(result[0] == 7 and stat.S_IMODE(permissions.stat().st_mode) == 0o750, 'existing private directory permissions stay unchanged', str(result))
            permissions.chmod(0o770)
            result = run(['run', entry])
            check(result[0] == 1 and 'cannot create private run cache' in result[2], 'group-writable cache rejected', str(result))
        print(f'workflow: {checks} checks passed')
    except BaseException:
        print('workflow artifacts:', work)
        raise
    else:
        shutil.rmtree(work)

if __name__ == '__main__':
    main()
