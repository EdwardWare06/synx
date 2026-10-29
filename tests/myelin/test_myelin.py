"""Myelin manifests, local/Git graphs, offline builds, and editor integration.

Uses isolated local Git repositories; no remote network access is required.
"""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import tomllib
from urllib.parse import urlparse
from urllib.request import url2pathname

ROOT = Path(__file__).resolve().parents[2]
EXE = '.exe' if os.name == 'nt' else ''
MODES = [[], ['--no-regalloc'], ['--release'], ['--release', '--no-regalloc']]
checks = 0


def check(value, label, detail=''):
    global checks
    checks += 1
    if not value:
        raise AssertionError(label + '\n' + str(detail))


def run(program, args, cwd, env, code=0):
    env = dict(env)
    if os.environ.get('SYNX_MYELIN_ASAN'):
        env.update(LD_PRELOAD=os.environ['SYNX_MYELIN_ASAN'], ASAN_OPTIONS='detect_leaks=1:halt_on_error=1')
    p = subprocess.run([str(program), *map(str, args)], cwd=cwd, env=env,
                       capture_output=True, timeout=900)
    out = p.stdout.decode('utf-8', 'replace').replace('\r\n', '\n')
    err = p.stderr.decode('utf-8', 'replace').replace('\r\n', '\n')
    check(p.returncode == code, f'{args}: exit {code}', (p.returncode, out, err))
    return out, err


def put(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode() if isinstance(text, str) else text)


def package(path, name, body='pub int value() { return 42; }', deps='', app=False):
    put(path / 'synx.toml', f'[package]\nname = "{name}"\nversion = "1.0.0"\n\n[dependencies]\n{deps}')
    put(path / 'src' / ('main.snx' if app else 'lib.snx'), body)


def manifests(synx, work, env):
    good = [
        '[package]\nname = "demo"\n',
        '# comment\r\n[package]\r\nname = \'demo\' # tail\r\nversion = "1.2.3"\r\n',
        '[package]\nname="demo"\n[dependencies]\nmath = { path = "../math" }\n',
        '[package]\nname="demo"\n[dependencies.math]\ngit="https://example.invalid/math"\nbranch="main"\n',
        '[package]\n"name"="de\\u006do"\n[bin]\npath="code/entry.snx"\n',
        '[package]\nname="demo"\n[lib]\npath="src/lib.snx"\n[dependencies]\nz={path="../z"}\na={path="../a"}',
        '[package]\nname="demo"\n[dependencies]\na = {git="https://example.invalid/a#fragment",tag="v1",subdir="lib/a"}',
        '[package]\nname="demo"\n[dependencies]\na={path="../caf\\U000000e9"}',
    ]
    bad = [
        '', '[package]\n', '[unknown]\n[package]\nname="demo"',
        '[package]\nname="demo"\nname="again"',
        '[package]\nname="demo"\n[package]\nversion="1.0.0"',
        '[package]\nname="demo"\n[dependencies]\na={path="a",path="b"}',
        '[package]\nname="demo"\n[dependencies]\na={path="a"}\na={git="b"}',
        '[package]\nname="demo"\n[dependencies]\na={path="a"}\n[dependencies.a]\ngit="b"',
        '[package]\nname="demo"\n[dependencies.a]\n',
        '[package]\nname="std"', '[package]\nname="wrong-name"',
        '[package]\nname="fn"', '[package]\nname="demo"\n[dependencies]\nfor={path="a"}',
        '[package]\nname="demo"\nversion="01.0.0"', '[package]\nname="demo"\nversion="1.0"',
        '[package]\nname="demo"\n[dependencies]\na="1.0.0"',
        '[package]\nname="demo"\n[dependencies]\na={path="a",git="b"}',
        '[package]\nname="demo"\n[dependencies]\na={path="a",branch="b"}',
        '[package]\nname="demo"\n[dependencies]\na={git="b",branch="b",tag="v1"}',
        '[package]\nname="demo"\n[dependencies]\na={git="ext::evil"}',
        '[package]\nname="demo"\n[dependencies]\na={git="-bad"}',
        '[package]\nname="demo"\n[dependencies]\na={git="url",rev="-bad"}',
        '[package]\nname="demo"\n[dependencies]\na={git="url",subdir="../escape"}',
        '[package]\nname="demo"\n[bin]\npath="../main.snx"',
        '[package]\nname="demo"\n[lib]\npath="C:/main.snx"',
        '[package]\nname="demo"\nunknown="x"',
        '[package]\nname="demo"\n[dependencies]\nstd={path="a"}',
        '[package]\nname="de\\qmo"', '[package]\nname="demo"\n#bad\x01',
        '[package]\nname="de\\uD800mo"', '[package]\nname="de\\Uffffffffmo"',
        '[package]\nname="de\\u0000mo"', '[package]\nname="demo"\r',
        b'[package]\nname="demo"\xff', '[package]\nname="demo"\n[dependencies]\na={}',
    ]
    for mode, flags in enumerate(MODES):
        probe = work / f'manifest-{mode}{EXE}'
        run(synx, ['build', ROOT / 'selfhost/myelin_probe.snx', '-o', probe, *flags], work, env)
        for i, data in enumerate(good):
            expected = tomllib.loads(data)
            file = work / 'manifest-input'; put(file, data)
            out, _ = run(probe, [file], work, env)
            check(out.startswith('demo '), f'manifest {mode}/{i}: decoded name', out)
            aliases = [line.split('|')[0] for line in out.splitlines()[1:]]
            check(aliases == sorted(expected.get('dependencies', {})), f'manifest {mode}/{i}: sorted dependencies', out)
        for i, data in enumerate(bad):
            file = work / 'manifest-input'; put(file, data)
            out, _ = run(probe, [file], work, env, 1)
            check(bool(out.strip()), f'manifest {mode}/{i}: error message')


def local(synx, work, env):
    app, lib = work / 'app', work / 'math'
    run(synx, ['new', app], work, env)
    run(synx, ['new', lib, '--lib'], work, env)
    check(tomllib.loads((app / 'synx.toml').read_text())['package']['name'] == 'app', 'new manifest')
    original = (app / 'synx.toml').read_bytes()
    run(synx, ['new', app], work, env, 1)
    check((app / 'synx.toml').read_bytes() == original, 'new preserves existing project')
    run(synx, ['add', 'math', '--path', '../math'], app, env)
    put(app / 'src/main.snx', 'import math.answer; int main() { return answer(); }')
    run(synx, ['check'], app, env, 1)
    run(synx, ['fetch', '--offline'], app, env)
    lock = (app / 'synx.lock').read_bytes(); parsed = json.loads(lock)
    check(parsed['packages'][0]['dependencies'] == {'math': 1}, 'lock dependency edge', parsed)
    check(str(work) not in lock.decode(), 'lock contains no resolved absolute cache/project paths')
    stamp = (app / 'synx.lock').stat().st_mtime_ns
    run(synx, ['fetch', '--locked', '--offline'], app, env)
    check((app / 'synx.lock').stat().st_mtime_ns == stamp, 'unchanged lock retains mtime')
    for file in (app / 'synx.toml', lib / 'synx.toml'):
        put(file, file.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
    run(synx, ['fetch', '--locked', '--offline'], app, env)
    check((app / 'synx.lock').read_bytes() == lock, 'CRLF manifests preserve portable lock')
    no_git = dict(env, PATH=str(work / 'empty-path'))
    run(synx, ['check'], app, no_git)
    out, _ = run(synx, ['check', '--json'], app, no_git)
    check(json.loads(out)['ok'], 'project JSON check')
    for flags in MODES:
        run(synx, ['run', '--locked', '--offline', *flags], app, no_git, 42)
    run(synx, ['build', '--offline'], app, no_git)
    check((app / 'build/debug' / f'app{EXE}').is_file(), 'project output directory')
    run(synx, ['build', '--release', '--locked', '--offline'], app, no_git)
    check((app / 'build/release' / f'app{EXE}').is_file(), 'release output directory')
    run(synx, ['check'], lib, no_git)
    # Project discovery and --path use the invocation directory.
    package(work / 'extra', 'extra')
    run(synx, ['add', 'extra', '--path', '../../extra'], app / 'src', env)
    added = tomllib.loads((app / 'synx.toml').read_text())
    check(added['dependencies']['extra']['path'] == '../extra', 'add records manifest-relative path', added)
    run(synx, ['fetch', '--offline'], app / 'src', env)
    lock = (app / 'synx.lock').read_bytes()
    # Edits to source remain live and invalidate the native run cache.
    put(lib / 'src/lib.snx', 'pub int answer() { return 43; }')
    run(synx, ['run', '--offline'], app, no_git, 43)
    put(lib / 'synx.toml', (lib / 'synx.toml').read_text() + '\n# manifest edit\n')
    run(synx, ['build', '--locked', '--offline'], app, env, 1)
    out, _ = run(synx, ['check', '--json'], app, no_git, 1)
    check(not json.loads(out)['ok'] and 'synx.lock' in json.loads(out)['error'], 'project errors retain JSON contract', out)
    check((app / 'synx.lock').read_bytes() == lock, 'locked failure preserves lock')
    run(synx, ['fetch', '--offline'], app, no_git)
    run(synx, ['check'], app, no_git)
    # Two transitive versions share names and module paths without sharing defs.
    package(work / 'v1', 'common', 'import helper.value; pub int get() { return value(); }')
    package(work / 'v2', 'common', 'import helper.value; pub int get() { return value(); }')
    put(work / 'v1/src/helper.snx', 'pub int value() { return 10; }')
    put(work / 'v2/src/helper.snx', 'pub int value() { return 20; }')
    package(work / 'left', 'left', 'import common.get; pub int value() { return get(); }', 'common={path="../v1"}\n')
    package(work / 'right', 'right', 'import common.get; pub int value() { return get(); }', 'common={path="../v2"}\n')
    package(work / 'diamond', 'diamond', 'import left; import right; int main() { return left.value() + right.value(); }', 'left={path="../left"}\nright={path="../right"}\n', True)
    run(synx, ['run', '--offline'], work / 'diamond', env, 30)
    # Undeclared transitive packages are inaccessible from the application.
    put(work / 'diamond/src/main.snx', 'import common; int main() { return common.get(); }')
    _, err = run(synx, ['check'], work / 'diamond', env, 1)
    check('cannot find imported module' in err, 'transitive visibility', err)
    package(work / 'cycle_a', 'cycle_a', deps='b={path="../cycle_b"}\n')
    package(work / 'cycle_b', 'cycle_b', deps='a={path="../cycle_a"}\n')
    _, err = run(synx, ['fetch', '--offline'], work / 'cycle_a', env, 1)
    check('cycle' in err, 'cycle diagnostic', err)
    run(synx, ['add', 'math', '--path', '../math'], app, env, 1)
    run(synx, ['add', 'other', '--path', '../math', '--git', 'invalid'], app, env, 1)


def git_sources(synx, work, env):
    repository = work / 'git source'
    package(repository, 'remote', 'pub int value() { return 61; }')
    git = shutil.which('git')
    check(bool(git), 'Git available for isolated fixtures')
    git_env = dict(env, GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull, GIT_AUTHOR_NAME='Synx Test', GIT_AUTHOR_EMAIL='test@example.invalid',
                   GIT_COMMITTER_NAME='Synx Test', GIT_COMMITTER_EMAIL='test@example.invalid')
    for args in (['init', '-q', '-b', 'main', repository], ['-C', repository, 'add', '.'], ['-C', repository, 'commit', '-qm', 'fixture one']):
        run(git, args, work, git_env)
    rev1, _ = run(git, ['-C', repository, 'rev-parse', 'HEAD'], work, git_env)
    app = work / 'gitapp'
    package(app, 'gitapp', 'import remote.value; int main() { return value(); }',
            f'remote={{git={json.dumps(repository.as_uri())},branch="main"}}\n', True)
    run(synx, ['fetch'], app, env)
    lock = (app / 'synx.lock').read_bytes()
    check(json.loads(lock)['pins'][0]['rev'] == rev1.strip(), 'Git pin records exact commit')
    no_git = dict(env, PATH=str(work / 'empty-path'))
    run(synx, ['run', '--offline', '--locked'], app, no_git, 61)
    put(repository / 'src/lib.snx', 'pub int value() { return 62; }')
    run(git, ['-C', repository, 'add', '.'], work, git_env)
    run(git, ['-C', repository, 'commit', '-qm', 'fixture two'], work, git_env)
    run(synx, ['fetch'], app, env)
    check((app / 'synx.lock').read_bytes() == lock, 'moving branch does not alter locked revision')
    run(synx, ['run'], app, no_git, 61)
    # A fresh cache must reconstruct the pinned commit, even after branch moves.
    fresh = dict(env, SYNX_MYELIN_CACHE=str(work / 'fresh-cache'))
    run(synx, ['fetch', '--offline', '--locked'], app, fresh, 1)
    run(synx, ['fetch', '--locked'], app, fresh)
    run(synx, ['run', '--offline', '--locked'], app, dict(fresh, PATH=no_git['PATH']), 61)
    check((app / 'synx.lock').read_bytes() == lock, 'locked fetch preserves lock bytes')
    original_manifest = (app / 'synx.toml').read_bytes()
    put(app / 'synx.toml', original_manifest + ('\n[dependencies.unpinned]\ngit=' + json.dumps(repository.as_uri()) + '\n').encode())
    _, err = run(synx, ['fetch', '--locked'], app, no_git, 1)
    check('no locked revision' in err, 'locked mode rejects unpinned sources before launching Git', err)
    check((app / 'synx.lock').read_bytes() == lock, 'unpinned locked failure preserves lock')
    put(app / 'synx.toml', original_manifest)
    # Following a definition opens an overlay for a managed Git source.
    spec = importlib.util.spec_from_file_location('synx_git_lsp', ROOT / 'tests/lsp/test_lsp.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    client = module.Client(synx, ['lsp'], {'SYNX_MYELIN_CACHE': env['SYNX_MYELIN_CACHE']})
    source = app / 'src/main.snx'; data = source.read_text()
    try:
        client.rpc('initialize', dict(capabilities={}, initializationOptions={'files': [source.as_uri()]}))
        client.notify('initialized', {}); client.open(source, data); client.barrier()
        check(not client.diagnostics(source)['diagnostics'], 'Git LSP graph loads', client.diagnostics(source))
        definition = client.query('definition', source, data, 'value', nth=1)
        location = definition[0] if isinstance(definition, list) else definition
        dependency = Path(url2pathname(urlparse(location['uri']).path)).resolve()
        client.open(dependency, dependency.read_text()); client.barrier()
        check(not client.diagnostics(source)['diagnostics'], 'opening Git dependency preserves app imports', client.diagnostics(source))
        client.rpc('shutdown'); client.notify('exit'); client.process.stdin.close()
        check(client.process.wait(timeout=120) == 0, 'Git LSP shuts down')
    finally:
        if client.process.poll() is None:
            client.process.kill(); client.process.wait()
    # Invalid locks fail before trying to invoke Git.
    parsed = json.loads(lock); parsed['pins'][0]['rev'] = '../escape'
    put(app / 'synx.lock', json.dumps(parsed))
    _, err = run(synx, ['fetch', '--offline'], app, env, 1)
    check('invalid Git pin' in err, 'invalid lock revision')
    put(app / 'synx.lock', lock)
    # A tagged package in a repository subdirectory resolves independently.
    nested = repository / 'nested'
    package(nested, 'nested', 'pub int value() { return 73; }')
    run(git, ['-C', repository, 'add', '.'], work, git_env)
    run(git, ['-C', repository, 'commit', '-qm', 'nested fixture'], work, git_env)
    run(git, ['-C', repository, 'tag', 'v1'], work, git_env)
    tagged = work / 'tagged'
    package(tagged, 'tagged', 'import nested.value; int main() { return value(); }',
            f'nested={{git={json.dumps(repository.as_uri())},tag="v1",subdir="nested"}}\n', True)
    run(synx, ['run', '--locked'], tagged, env, 1)
    check(not (tagged / 'synx.lock').exists(), 'locked missing lock does not create a lock')
    run(synx, ['run'], tagged, env, 73)
    run(synx, ['run', '--offline'], tagged, no_git, 73)
    # Failed source resolution must leave an existing lock intact.
    tagged_lock = (tagged / 'synx.lock').read_bytes()
    put(tagged / 'synx.toml', (tagged / 'synx.toml').read_text().replace('tag="v1"', 'tag="missing"'))
    run(synx, ['fetch'], tagged, env, 1)
    check((tagged / 'synx.lock').read_bytes() == tagged_lock, 'failed fetch preserves lock')


def limits(synx, work, env):
    # Reusing an existing node at the package cap must remain valid.
    deps = []
    for i in range(1023):
        name = f'limit_p{i:04}'
        package(work / name, name)
        deps.append(f'p{i:04}={{path="../{name}"}}\n')
    deps.append('zz_shared={path="../limit_p0000"}\n')
    app = work / 'limit_app'
    package(app, 'limit_app', 'int main() { return 0; }', ''.join(deps), True)
    run(synx, ['fetch', '--offline'], app, env)
    lock = (app / 'synx.lock').read_bytes()
    check(len(json.loads(lock)['packages']) == 1024, 'package cap deduplicates shared nodes')
    package(work / 'limit_extra', 'limit_extra')
    put(app / 'synx.toml', (app / 'synx.toml').read_text() + 'zz_extra={path="../limit_extra"}\n')
    _, err = run(synx, ['fetch', '--offline'], app, env, 1)
    check('1024' in err, 'package cap error', err)
    check((app / 'synx.lock').read_bytes() == lock, 'package cap preserves prior lock')
    for i in range(66):
        package(work / f'depth_{i}', f'depth_{i}', deps=f'child={{path="../depth_{i+1}"}}\n' if i < 65 else '')
    _, err = run(synx, ['fetch', '--offline'], work / 'depth_0', env, 1)
    check('64' in err, 'dependency depth limit', err)


def editor(synx, work, env):
    spec = importlib.util.spec_from_file_location('synx_lsp_tests', ROOT / 'tests/lsp/test_lsp.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    app = work / 'app'; source = app / 'src/main.snx'; data = source.read_text()
    # Client creates a clean environment, so this case uses only path sources.
    client = module.Client(synx, ['lsp'])
    try:
        client.rpc('initialize', dict(capabilities={}, initializationOptions={'files': [source.as_uri()]}))
        client.notify('initialized', {})
        client.open(source, data); client.barrier()
        check(not client.diagnostics(source)['diagnostics'], 'LSP project imports', client.diagnostics(source))
        definition = client.query('definition', source, data, 'answer', nth=1)
        check('math/src/lib.snx' in json.dumps(definition), 'LSP dependency definition', definition)
        manifest = app / 'synx.toml'; before = manifest.read_bytes(); put(manifest, before + b'\n# editor manifest edit\n')
        client.notify('workspace/didChangeWatchedFiles', dict(changes=[dict(uri=manifest.as_uri(), type=2)])); client.barrier()
        check(any(d.get('code') == 'SNX0305' for d in client.diagnostics(source)['diagnostics']), 'LSP stale package graph', client.diagnostics(source))
        run(synx, ['fetch', '--offline'], app, env)
        client.notify('workspace/didChangeWatchedFiles', dict(changes=[dict(uri=(app / 'synx.lock').as_uri(), type=2)])); client.barrier()
        check(not client.diagnostics(source)['diagnostics'], 'LSP refreshed package graph', client.diagnostics(source))
        client.rpc('shutdown'); client.notify('exit'); client.process.stdin.close()
        check(client.process.wait(timeout=120) == 0, 'LSP shuts down')
    finally:
        if client.process.poll() is None:
            client.process.kill(); client.process.wait()


def main():
    synx = Path(sys.argv[1]).resolve()
    env = {k: v for k, v in os.environ.items() if not k.startswith('SYNX_')}
    env.update(SYNX_STD_PATH=str(ROOT / 'std'), SYNX_LINKER='builtin')
    work = Path(tempfile.mkdtemp(prefix='synx-myelin-')).resolve()
    env.update(SYNX_MYELIN_CACHE=str(work / 'cache'), SYNX_CACHE_DIR=str(work / 'run-cache'))
    try:
        manifests(synx, work, env)
        local(synx, work, env)
        limits(synx, work, env)
        if '--memory' not in sys.argv[2:]:
            git_sources(synx, work, env)
        editor(synx, work, env)
    except BaseException:
        print(f'artifacts: {work}', flush=True)
        raise
    else:
        check(work.parent == Path(tempfile.gettempdir()).resolve() and work.name.startswith('synx-myelin-'), 'cleanup stays inside generated temporary directory')
        def writable_remove(function, path, error):
            # Git marks packed objects read-only on Windows.
            Path(path).resolve().relative_to(work)
            os.chmod(path, stat.S_IREAD | stat.S_IWRITE)
            function(path)
        shutil.rmtree(work, onerror=writable_remove)
    print(f'myelin: {checks} checks passed')


if __name__ == '__main__':
    main()
