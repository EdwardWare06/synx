"""Native variable/type acceptance through GDB or the Windows debug API."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
checks = 0


def check(ok, message):
    global checks
    checks += 1
    if not ok:
        raise AssertionError(message)


def run(args, env):
    p = subprocess.run(list(map(str, args)), env=env, capture_output=True, timeout=900)
    check(p.returncode == 0, f'{args}: {p.returncode}\n{p.stdout!r}\n{p.stderr!r}')
    return p.stdout.decode('utf8', 'replace') + p.stderr.decode('utf8', 'replace')


def gdb_script(source):
    markers = {}
    for line, text in enumerate(source.read_text().splitlines(), 1):
        for name in ('VALUES', 'SHADOW', 'OUTER', 'INNER', 'RESULT'):
            if '// ' + name in text:
                markers[name] = line
    commands = ['set pagination off', 'set debuginfod enabled off', 'set breakpoint pending on']
    commands += [f'break variables.snx:{markers[name]}' for name in markers]
    commands += ['run', 'python', 'import gdb',
                 'v = gdb.parse_and_eval',
                 'assert int(v("value")) == 8', 'assert int(v("parameter")) == 6',
                 'assert int(v("total")) == 3', 'assert int(v("point.x")) == 42',
                 'assert int(v("point.label")) == 7', 'assert float(v("point.y")) == 2.5',
                 'assert int(v("sizeof(point)")) == 16',
                 'assert int(v("pair.second")) == 13', 'assert int(v("numbers[1]")) == 29',
                 'assert int(v("choice.tag")) == 1',
                 'assert int(v("choice.payload.Pair._0")) == 17',
                 'assert int(v("choice.payload.Pair._1")) == 19',
                 'assert int(v("list.payload.Next._1.data").dereference()["tag"]) == 0',
                 'assert v("original").is_optimized_out',
                 'print("SYNX_GDB_VALUES_OK")', 'end']
    # Marker insertion order follows the source, so INNER is breakpoint 1.
    order = list(markers)
    commands += [f'disable {order.index("VALUES") + 1}', 'continue',
                 'python assert int(v("value")) == 91',
                 f'disable {order.index("SHADOW") + 1}', 'continue',
                 'python assert int(v("value")) == 8',
                 f'disable {order.index("OUTER") + 1}', 'continue',
                 'python', 'assert int(v("scratch")) == 56', 'assert int(v("other")) == 108',
                 'gdb.selected_frame().older().select()', 'assert int(v("value")) == 8',
                 'gdb.newest_frame().select()', 'print("SYNX_GDB_CALLER_OK")', 'end',
                 f'disable {order.index("INNER") + 1}', 'continue',
                 'python assert int(v("value")) == 8 and int(v("result")) == 56',
                 f'disable {order.index("RESULT") + 1}', 'continue']
    return '\n'.join(commands) + '\n'


def flow_script(source):
    markers = {}
    for line, text in enumerate(source.read_text().splitlines(), 1):
        for name in ('JOIN', 'LOOP', 'LOOP_END', 'MATCH', 'MATCH_END', 'BOX', 'HIDDEN'):
            if text.rstrip().endswith('// ' + name):
                markers[name] = line
    commands = ['set pagination off', 'set debuginfod enabled off', 'set breakpoint pending on',
                'break reuse', 'run', 'python', 'import gdb', 'v = gdb.parse_and_eval',
                'samples = 0',
                'while gdb.selected_frame().name() == "reuse":',
                '    argument = v("argument")',
                '    if not argument.is_optimized_out:',
                '        assert int(argument) == 8, "stale register after reuse at %s: %s" % (v("$pc"), argument)',
                '    samples += 1', '    assert samples < 1000',
                '    gdb.execute("nexti", to_string=True)',
                'assert samples > 10', 'print("SYNX_GDB_REUSE_OK")', 'end', 'disable 1']
    for name, line in markers.items():
        commands.append(f'break flow.snx:{line}')
    expressions = [
        'int(v("initialized")) == 41 and v("original").is_optimized_out',
        'int(v("value")) == 0', 'int(v("value")) == 55',
        'int(v("value")) == 77', 'int(v("value")) == 55',
        'int(v("extracted")) == 81 and v("boxed").is_optimized_out',
        'v("name").is_optimized_out',
    ]
    for i, expression in enumerate(expressions, 2):
        commands += ['continue', f'python assert {expression}', f'disable {i}']
    commands += ['python print("SYNX_GDB_FLOW_OK")', 'continue']
    return '\n'.join(commands) + '\n'


def main():
    synx, runtime, harness = map(lambda p: Path(p).resolve(), sys.argv[1:4])
    if sys.platform == 'darwin':
        print('Variable debug acceptance runs on Linux and Windows; macOS debug output is separate work.')
        return
    env = {k: v for k, v in os.environ.items() if not k.startswith('SYNX_')}
    env.update(SYNX_STD_PATH=str(ROOT / 'std'), SYNX_LINKER='builtin',
               SYNX_RUNTIME_HOST=str(runtime / ('synx-host.exe' if os.name == 'nt' else 'synx-host')),
               SYNX_RUNTIME_LIB=str(runtime / ('synx_rt.lib' if os.name == 'nt' else 'libsynx_rt.a')),
               SYNX_RUNTIME_SHARED=str(runtime / ('synx_rt.dll' if os.name == 'nt' else 'libsynx_rt.so')))
    memory = '--memory' in sys.argv
    if memory:
        env.update(LD_PRELOAD=os.environ['SYNX_VARIABLES_ASAN'], ASAN_OPTIONS='detect_leaks=1:halt_on_error=1')
    with tempfile.TemporaryDirectory(prefix='synx-variables-') as tmp:
        work = Path(tmp)
        if os.name == 'nt':
            from test_pdb import Pdb
            import struct
            wide = work / 'wide.snx'
            wide.write_text('struct Wide {\n' + ''.join(f'int field_{i};\n' for i in range(4096)) +
                            '}\nint main() { Wide value; return 0; }\n', encoding='utf8')
            exe = work / 'wide.exe'
            run([synx, 'build', wide, '-o', exe, '-g'], env)
            run([exe], env)
            pdb = Pdb(exe.with_suffix('.pdb').read_bytes())
            index = next(struct.unpack_from('<I', raw, 8)[0] for kind, raw in pdb.types.values()
                         if kind == 0x1505 and struct.unpack_from('<H', raw, 4)[0] == 4096)
            fields, continuations = [], 0
            while index:
                kind, raw = pdb.types[index]
                check(kind == 0x1203, 'continued field list')
                at, index = 4, 0
                while at < len(raw):
                    leaf = struct.unpack_from('<H', raw, at)[0]
                    if leaf == 0x1404:
                        index = struct.unpack_from('<I', raw, at + 4)[0]
                        continuations += 1
                        break
                    check(leaf == 0x150d, 'data member in field list')
                    ty, offset = struct.unpack_from('<IH', raw, at + 4)
                    stop = raw.index(0, at + 10)
                    fields.append((raw[at + 10:stop].decode(), ty, offset))
                    at = (stop + 4) & ~3
            check(continuations > 0 and fields == [(f'field_{i}', 0x74, i * 4) for i in range(4096)],
                  'all large-struct fields, offsets and continuation order')
        source = ROOT / 'tests/debug/variables.snx'
        commands = work / 'variables.gdb'
        commands.write_text(gdb_script(source), encoding='utf8')
        for mode in range(4):
            flags = (['--no-regalloc'] if mode % 2 else []) + (['--release'] if mode >= 2 else [])
            for kind, linker in enumerate(([], ['--shared-runtime'], ['--linker=system'])):
                if memory and kind:
                    continue
                exe = work / (f'vars-{mode}-{kind}' + ('.exe' if os.name == 'nt' else ''))
                run([synx, 'build', source, '-o', exe, '-g', '--keep-obj', *flags, *linker], env)
                out = run([exe], env)
                check(out.splitlines() == ['values 8 6 42 13 29 3', 'shadow 91', 'outer 8', 'inner 56 108', 'result 56 value 8'], 'fixture output')
                if memory:
                    continue
                if os.name == 'nt':
                    out = run([harness / 'test_windows_variables.exe', exe, source], env)
                    check('SYNX_WINDOWS_VARIABLES_OK' in out, 'native Windows values, types, scopes and caller registers')
                else:
                    out = run(['gdb', '-q', '-batch', '-x', commands, exe], env)
                    check('Traceback' not in out and 'Error while executing Python code' not in out and
                          'SYNX_GDB_VALUES_OK' in out and 'SYNX_GDB_CALLER_OK' in out and 'exited normally' in out,
                          'GDB values, types, scopes, caller registers and completion\n' + out)
            flow_source = ROOT / 'tests/debug/flow.snx'
            flow = work / (f'flow-{mode}' + ('.exe' if os.name == 'nt' else ''))
            run([synx, 'build', flow_source, '-o', flow, '-g', *flags], env)
            run([flow], env)
            if sys.platform == 'linux' and not memory:
                commands.write_text(flow_script(flow_source), encoding='utf8')
                out = run(['gdb', '-q', '-batch', '-x', commands, flow], env)
                check('Traceback' not in out and 'SYNX_GDB_REUSE_OK' in out and 'SYNX_GDB_FLOW_OK' in out and
                      'exited normally' in out, 'GDB register reuse and control-flow availability\n' + out)
                commands.write_text(gdb_script(source), encoding='utf8')
    print(f'Variable/type debug checks: {checks}')


if __name__ == '__main__':
    main()
