"""Formatter golden output, publication safety, syntax preservation and corpus.

The compiler itself compares strict AST records before returning any edit.
Corpus files are copied to a temporary tree; repository sources stay unchanged.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
checks = 0


def check(value, message, detail=''):
    global checks
    checks += 1
    if not value:
        raise AssertionError(message + '\n' + str(detail))


def main():
    compiler = Path(sys.argv[1]).resolve()
    env = dict(os.environ)
    env['SYNX_STD_PATH'] = str(ROOT / 'std')
    if len(sys.argv) > 2:
        env['SYNX_RUNTIME_HOST'] = str(Path(sys.argv[2]).resolve())
    if os.environ.get('SYNX_FMT_ASAN'):
        env['LD_PRELOAD'] = os.environ['SYNX_FMT_ASAN']
        env['ASAN_OPTIONS'] = 'detect_leaks=1:halt_on_error=1'

    def run(*args, data=None):
        return subprocess.run([str(compiler), 'fmt', *map(str, args)], input=data,
                              capture_output=True, env=env, timeout=120)

    cases = [
        ('basic', b'int main(){int x=1+2;return x;}\n',
         b'int main() {\n    int x = 1 + 2;\n    return x;\n}\n'),
        ('blocks', b'main(){if(true){print("yes");}else{print("no");}}',
         b'main() {\n    if (true) {\n        print("yes");\n    } else {\n        print("no");\n    }\n}\n'),
        ('comments', b'// header\nmain(){int x=1;// keep\n/* nested /* block */ end */print("x");}',
         b'// header\nmain() {\n    int x = 1; // keep\n    /* nested /* block */ end */\n    print("x");\n}\n'),
        ('records', b'import std.fs.{File,IoError};\nstruct P{int x;int y;}\nmain(){var p=P{x:1,y:2};}',
         b'import std.fs.{File, IoError};\n\nstruct P {\n    int x;\n    int y;\n}\n\nmain() {\n    var p = P { x: 1, y: 2 };\n}\n'),
        ('operators', b'int f(int x,int y){return x>y&&x<5||x>>y==2;}\n',
         b'int f(int x, int y) {\n    return x > y && x < 5 || x >> y == 2;\n}\n'),
        ('generics', b'Box<Vec<int>> f<T:Ord+Eq>(Box<Vec<int>> x){return x;}\n',
         b'Box<Vec<int>> f<T: Ord + Eq>(Box<Vec<int>> x) {\n    return x;\n}\n'),
        ('literals', b'main(){var s="a { 1 +  2 } {{x}}";var c=\'\\n\';int n=0xF_F;}',
         b'main() {\n    var s = "a { 1 +  2 } {{x}}";\n    var c = \'\\n\';\n    int n = 0xF_F;\n}\n'),
        ('prefix', b'int f(&const int x,*const byte p){return - *x+!false as int;}',
         b'int f(&const int x, *const byte p) {\n    return -*x + !false as int;\n}\n'),
        ('receivers', b'struct S{int get(&const self){return 1;}void set(&self){}}',
         b'struct S {\n    int get(&const self) {\n        return 1;\n    }\n    void set(&self) {}\n}\n'),
        ('question-boundary', b'f(){return source()? .field;}\ng(){return source()? ?;}',
         b'f() {\n    return source()? .field;\n}\n\ng() {\n    return source()? ?;\n}\n'),
        ('closure', b'main(){var f=[&const x] const |int a,int b|a+b;var g=||{return 1;};}',
         b'main() {\n    var f = [&const x] const |int a, int b| a + b;\n    var g = || {\n        return 1;\n    };\n}\n'),
        ('ranges', b'main(){var a=[1,2,3];var s=&const a[1..=2];for(x in a){print("{x}");}}',
         b'main() {\n    var a = [1, 2, 3];\n    var s = &const a[1..=2];\n    for (x in a) {\n        print("{x}");\n    }\n}\n'),
        ('empty', b' \t\n', b''),
        ('comment-cr', b'// one\rtwo /* still line text */', b'// one\rtwo /* still line text */\n'),
        ('doc-cr', b'/// one\rtwo\nmain(){}', b'/// one\rtwo\nmain() {}\n'),
        ('unicode', 'main(){print("caf\u00e9 \U0001f600");}'.encode(),
         'main() {\n    print("caf\u00e9 \U0001f600");\n}\n'.encode()),
    ]
    with tempfile.TemporaryDirectory(prefix='synx fmt caf\u00e9 ') as folder:
        work = Path(folder)
        for name, source, expected in cases:
            result = run('--stdin', data=source)
            check(result.returncode == 0, name + ' formats', result.stderr.decode('utf8', 'replace'))
            check(result.stdout == expected, name + ' golden output', repr(result.stdout))
            repeat = run('--stdin', data=result.stdout)
            check(repeat.returncode == 0 and repeat.stdout == result.stdout, name + ' is idempotent', repeat.stderr)
            checked = run('--stdin', '--check', data=expected)
            check(checked.returncode == 0 and not checked.stdout, name + ' check accepts formatted input', checked.stderr)

        path = work / 'spaced file.snx'
        source, expected = cases[0][1:]
        path.write_bytes(source)
        result = run('--check', path)
        check(result.returncode == 1 and not result.stdout and path.read_bytes() == source, 'check is read-only', result.stderr)
        result = run('--stdout', path)
        check(result.returncode == 0 and result.stdout == expected and path.read_bytes() == source, 'stdout is read-only', result.stderr)
        result = run(path)
        check(result.returncode == 0 and path.read_bytes() == expected, 'atomic in-place update', result.stderr)
        stamp = path.stat().st_mtime_ns
        check(run(path).returncode == 0 and path.stat().st_mtime_ns == stamp, 'unchanged file keeps modification time')
        check(not list(work.glob('*.synx-fmt-*')), 'no publication temporary remains')
        for bad in [b'main(){var x=;}', b'main(){ /* unterminated', b'main(){print("\xff");}']:
            path.write_bytes(bad)
            result = run(path)
            check(result.returncode == 2 and path.read_bytes() == bad and not result.stdout, 'invalid input remains unchanged', result.stderr)
            result = run('--stdin', data=bad)
            check(result.returncode == 2 and not result.stdout, 'invalid stdin has no output', result.stderr)
        crlf = b'/// First\r\n/// Second\r\nmain(){print("x");}\r\n'
        result = run('--stdin', data=crlf)
        check(result.returncode == 0 and b'\r\n' in result.stdout and b'\r\r\n' not in result.stdout, 'CRLF stays CRLF', result.stderr)
        check(run('--stdin', data=result.stdout).stdout == result.stdout, 'CRLF is idempotent')
        mixed = b'/// First\r\n/// Second\nmain(){print("x");}\r\n'
        result = run('--stdin', data=mixed)
        check(result.returncode == 0 and result.stdout.startswith(b'/// First\r\n/// Second\n'), 'mixed doc endings preserved', result.stderr)
        check(run('--stdin', data=result.stdout).stdout == result.stdout, 'mixed endings are idempotent')
        wide = b'main(){call(' + b','.join([b'long_argument_name_' + str(i).encode() for i in range(8)]) + b');}'
        result = run('--stdin', data=wide)
        check(result.returncode == 0 and b'call(\n        long_argument' in result.stdout and max(map(len, result.stdout.splitlines())) <= 100, 'long argument list wraps', result.stdout)
        second = work / 'second.snx'; second.write_bytes(source); path.write_bytes(source)
        check(run(path, second).returncode == 0 and second.read_bytes() == expected, 'multiple files')
        check(run('--check', path, second).returncode == 0, 'multiple files check')
        for options in [[], ['--bogus'], ['--stdout', 'a', 'b'], ['--stdin', str(path)], ['--check', '--stdout', str(path)]]:
            check(run(*options).returncode == 2, 'invalid options fail: ' + str(options))
        check(run('--help').returncode == 0, 'formatter help')
        check(run(work / 'missing.snx').returncode == 2, 'missing input fails')

        if len(sys.argv) > 2:
            probe = work / ('publication.exe' if os.name == 'nt' else 'publication')
            built = subprocess.run([str(compiler), 'build', str(ROOT / 'tests/fmt/publication.snx'), '-o', str(probe)], capture_output=True, env=env, timeout=120)
            check(built.returncode == 0, 'native publication probe builds', built.stderr)
            old_file, new_file = work / 'expected', work / 'replacement'
            old_file.write_bytes(source); new_file.write_bytes(expected)
            def publish():
                result = subprocess.run([str(probe), str(path), str(old_file), str(new_file)], capture_output=True, env=env, timeout=30)
                check(b'AddressSanitizer' not in result.stderr, 'publication allocation check', result.stderr)
                return result.returncode
            path.write_bytes(source.replace(b'1+2', b'3+4'))
            changed = path.read_bytes()
            check(publish() != 0 and path.read_bytes() == changed, 'stale source cannot be replaced')
            path.write_bytes(source)
            check(publish() == 0 and path.read_bytes() == expected, 'native checked publication')
            path.write_bytes(source); path.chmod(0o444)
            try:
                check(publish() != 0 and path.read_bytes() == source, 'read-only source stays intact')
            finally:
                path.chmod(0o640)
            check(not list(work.glob('*.synx-fmt-*')), 'no temporary after native failures')

        if os.name != 'nt':
            path.write_bytes(source); path.chmod(0o640)
            check(run(path).returncode == 0 and path.stat().st_mode & 0o777 == 0o640, 'file mode preserved')
            path.write_bytes(source)
            link = work / 'link.snx'; link.symlink_to(path)
            check(run(link).returncode == 0 and link.is_symlink() and path.read_bytes() == expected, 'symlink target formatted')

        # Exercise every production construct without resolving imports or names.
        sources = sorted(p for directory in ['selfhost', 'std', 'examples', 'tests/run']
                         for p in (ROOT / directory).rglob('*.snx'))
        targets = []
        for i, original in enumerate(sources):
            target = work / f'corpus-{i}.snx'; target.write_bytes(original.read_bytes()); targets.append(target)
        for start in range(0, len(targets), 40):
            batch = targets[start:start+40]
            result = run(*batch)
            check(result.returncode == 0, 'corpus syntax preserved', result.stderr.decode('utf8', 'replace'))
            result = run('--check', *batch)
            check(result.returncode == 0, 'corpus is idempotent', result.stderr.decode('utf8', 'replace'))
        fixtures = sorted((ROOT / 'tests/fixtures').rglob('*.snx'))
        for start in range(0, len(fixtures), 40):
            batch = []
            for i, original in enumerate(fixtures[start:start+40]):
                target = work / f'fixture-{start+i}.snx'; target.write_bytes(original.read_bytes()); batch.append(target)
            result = run(*batch)
            # Negative syntax fixtures may fail parsing. A syntax-preservation
            # failure on accepted syntax is always a formatter defect.
            check(result.returncode in (0, 2) and b'formatter could not preserve syntax' not in result.stderr, 'fixture syntax preserved or rejected', result.stderr.decode('utf8', 'replace'))
            result = run('--check', *batch)
            check(result.returncode in (0, 2) and not any(line.startswith(b'would format: ') for line in result.stderr.splitlines()), 'fixture formatting is idempotent', result.stderr.decode('utf8', 'replace'))
        print(f'fmt: {checks} assertions, {len(sources)} production sources and {len(fixtures)} fixture inputs passed')


if __name__ == '__main__':
    main()
