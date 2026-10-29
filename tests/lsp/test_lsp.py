"""Black-box LSP protocol, semantic identity, unsaved imports and rename checks."""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
checks = 0

def check(value, label, detail=''):
    global checks
    checks += 1
    if not value:
        raise AssertionError(label + ('\n' + str(detail) if detail else ''))

class Client:
    def __init__(self, compiler, args, env_overrides=None):
        env = {k: v for k, v in os.environ.items() if not k.startswith('SYNX_')}
        env['SYNX_STD_PATH'] = str(ROOT / 'std')
        env.update(env_overrides or {})
        if os.environ.get('SYNX_LSP_ASAN'):
            env['LD_PRELOAD'] = os.environ['SYNX_LSP_ASAN']
            env['ASAN_OPTIONS'] = 'detect_leaks=1:halt_on_error=1'
        self.process = subprocess.Popen([str(compiler), *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.messages = queue.Queue()
        self.notifications = []
        self.next_id = 1
        threading.Thread(target=self.read, daemon=True).start()
    def read(self):
        try:
            while True:
                headers = {}
                while True:
                    line = self.process.stdout.readline()
                    if not line:
                        raise EOFError('server stdout closed')
                    if line == b'\r\n':
                        break
                    k, v = line.decode('ascii').strip().split(':', 1)
                    headers[k.lower()] = v.strip()
                n = int(headers['content-length'])
                data = self.process.stdout.read(n)
                if len(data) != n:
                    raise EOFError('truncated frame')
                self.messages.put(json.loads(data))
        except Exception as error:
            self.messages.put(error)
    def raw(self, body, fragment=False):
        frame = b'Content-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body
        if fragment:
            for b in frame:
                self.process.stdin.write(bytes([b])); self.process.stdin.flush()
        else:
            self.process.stdin.write(frame); self.process.stdin.flush()
    def send(self, message, fragment=False):
        self.raw(json.dumps(dict(jsonrpc='2.0', **message), ensure_ascii=False).encode(), fragment)
    def notify(self, method, params=None):
        self.send(dict(method=method, params=params))
    def rpc(self, method, params=None, error=False, id=None):
        if id is None:
            id = self.next_id; self.next_id += 1
        self.send(dict(id=id, method=method, params=params))
        return self.response(id, error)
    def response(self, id, error=False):
        deadline = time.monotonic() + 120
        while True:
            value = self.messages.get(timeout=max(0.01, deadline-time.monotonic()))
            if isinstance(value, Exception):
                raise value
            if 'method' in value:
                self.notifications.append(value); continue
            check(value.get('id') == id, 'response preserves request ID', value)
            check(('error' in value) == error, 'expected response status', value)
            return value.get('error' if error else 'result')
    def barrier(self):
        self.rpc('unknown/barrier', error=True)
    def open(self, file, text, version=1):
        self.notify('textDocument/didOpen', dict(textDocument=dict(uri=file.as_uri(), languageId='synx', version=version, text=text)))
    def change(self, file, text, version):
        self.notify('textDocument/didChange', dict(textDocument=dict(uri=file.as_uri(), version=version), contentChanges=[dict(text=text)]))
    def query(self, method, file, text, needle, nth=0, delta=0, **extra):
        offset = -1
        for _ in range(nth+1):
            offset = text.index(needle, offset+1)
        offset += delta
        line = text.count('\n', 0, offset)
        line_start = text.rfind('\n', 0, offset)+1
        character = len(text[line_start:offset].encode('utf-16-le'))//2
        return self.rpc('textDocument/'+method, dict(textDocument=dict(uri=file.as_uri()), position=dict(line=line, character=character), **extra))
    def diagnostics(self, file):
        for notification in reversed(self.notifications):
            p = notification.get('params', {})
            if notification.get('method') == 'textDocument/publishDiagnostics' and p['uri'].lower() == file.as_uri().lower():
                return p
        raise AssertionError('missing diagnostics for '+str(file))
    def close(self):
        self.process.kill(); self.process.wait(timeout=10)
        return self.process.stderr.read().decode('utf8', 'replace')

def main():
    compiler = Path(sys.argv[1]).resolve()
    args = ['lsp']
    with tempfile.TemporaryDirectory(prefix='synx lsp caf\u00e9 ') as work:
        folder = Path(work).resolve()
        main_file, helper, other = [folder / name for name in ['main.snx', 'helper.snx', 'other.snx']]
        helper_text = '/// Adds two values.\npub int sum(int left, int right) { return left + right; }\n'
        helper.write_text(helper_text, encoding='utf8')
        main_text = 'import helper;\nint main() { int value = 2; { int value = 3; print("{value}"); } return helper.sum(value, 4); }\n'
        main_file.write_text(main_text, encoding='utf8')
        other_text = 'import helper;\nint main() { return helper.sum(1, 2); }\n'
        other.write_text(other_text, encoding='utf8')
        c = Client(compiler, args)
        try:
            e = c.rpc('textDocument/hover', {}, error=True)
            check(e['code'] == -32002, 'initialize required')
            caps = c.rpc('initialize', dict(capabilities={}, initializationOptions=dict(files=[f.as_uri() for f in [main_file, helper, other]])), id='init')
            check(caps['capabilities']['positionEncoding'] == 'utf-16', 'UTF-16 positions')
            check(caps['capabilities']['renameProvider']['prepareProvider'], 'rename preparation')
            check(caps['capabilities']['documentFormattingProvider'], 'document formatting capability')
            c.notify('initialized', {})
            if os.name == 'nt':
                encoded = main_file.as_uri().replace(':', '%3A', 2).replace('file%3A', 'file:', 1)
                c.notify('textDocument/didOpen', dict(textDocument=dict(uri=encoded, languageId='synx', version=1, text=main_text)))
                c.barrier(); check(c.diagnostics(main_file)['diagnostics'] == [], 'percent-encoded Windows drive colon')
                c.notify('textDocument/didClose', dict(textDocument=dict(uri=encoded)))
                c.barrier()
            c.open(main_file, main_text); c.barrier()
            check(c.diagnostics(main_file)['diagnostics'] == [], 'clean document diagnostics', c.notifications)
            formatting = c.rpc('textDocument/formatting', dict(textDocument=dict(uri=main_file.as_uri()), options=dict(tabSize=2, insertSpaces=False)))
            check(len(formatting) == 1 and '\n    int value = 2;' in formatting[0]['newText'], 'formatter returns the fixed Synx style', formatting)
            check(main_file.read_text() == main_text, 'formatting does not write disk sources')
            h = c.query('hover', main_file, main_text, 'sum')
            check('int sum(int left, int right)' in h['contents']['value'] and 'Adds two values' in h['contents']['value'], 'function hover and docs', h)
            d = c.query('definition', main_file, main_text, 'sum')
            check(d['uri'].lower() == helper.as_uri().lower() and d['range']['start']['line'] == 1, 'definition across import', d)
            refs = c.query('references', main_file, main_text, 'sum', context=dict(includeDeclaration=True))
            check(len(refs) == 3 and any(r['uri'].lower() == other.as_uri().lower() for r in refs), 'references include unopened importer', refs)
            refs = c.query('references', main_file, main_text, 'value', nth=0, context=dict(includeDeclaration=True))
            check(len(refs) == 2, 'shadowed variables have distinct identities', refs)
            rename = c.query('rename', main_file, main_text, 'sum', newName='combine')
            check(len(rename['documentChanges']) == 3, 'rename edits all resolved files', rename)
            check(all(e['newText'] == 'combine' for f in rename['documentChanges'] for e in f['edits']), 'rename replacement text')
            check(main_file.read_text() == main_text, 'server never writes source files')
            sig = c.query('signatureHelp', main_file, main_text, '4);', delta=0)
            check(sig['activeParameter'] == 1 and len(sig['signatures'][0]['parameters']) == 2, 'function argument hints', sig)
            c.open(helper, helper_text.replace('left + right', 'left + missing'))
            c.barrier(); check(any('missing' in d['message'] for d in c.diagnostics(helper)['diagnostics']), 'unsaved imported text is checked')
            c.change(helper, helper_text, 2); c.barrier()
            check(c.diagnostics(helper)['diagnostics'] == [], 'unsaved import fix clears diagnostics')
            c.change(helper, 'bad old text', 1); c.barrier()
            check(c.diagnostics(helper)['diagnostics'] == [], 'stale document version ignored')
            # UTF-16 units include two units for a non-BMP code point.
            unicode_text = 'int main() { var text = "caf\u00e9 \U0001f600"; return missing; }\r\n'
            c.change(main_file, unicode_text, 2); c.barrier()
            diag = next(d for d in c.diagnostics(main_file)['diagnostics'] if 'missing' in d['message'])
            expected = len(unicode_text[:unicode_text.index('missing')].encode('utf-16-le'))//2
            check(diag['range']['start']['character'] == expected, 'UTF-16 diagnostic after emoji', diag)
            formatting = c.rpc('textDocument/formatting', dict(textDocument=dict(uri=main_file.as_uri()), options=dict(tabSize=4, insertSpaces=True)))
            check(len(formatting) == 1 and 'return missing;' in formatting[0]['newText'] and 'caf\u00e9 \U0001f600' in formatting[0]['newText'], 'formatting uses unsaved text with type errors', formatting)
            check(formatting[0]['range']['end'] == dict(line=1, character=0), 'formatting range covers CRLF buffer', formatting)
            c.notify('textDocument/didChange', dict(textDocument=dict(uri=main_file.as_uri(), version=3), contentChanges=[dict(range=dict(start=dict(line=0, character=expected), end=dict(line=0, character=expected+7)), text='0')]))
            c.barrier(); check(c.diagnostics(main_file)['diagnostics'] == [], 'incremental edit uses UTF-16 positions')
            # An unsaved module can exist only in the editor.
            ghost = folder / 'ghost.snx'
            c.open(ghost, 'pub int answer() { return 9; }')
            c.change(main_file, 'import ghost; int main() { return ghost.answer(); }', 4); c.barrier()
            check(c.diagnostics(main_file)['diagnostics'] == [], 'new unsaved module resolves', c.diagnostics(main_file))
            # Syntax recovery must retain independent functions.
            partial = 'int stable() { return 4; }\nint main() { int broken = ; return stable(); }'
            c.change(main_file, partial, 5); c.barrier()
            h = c.query('hover', main_file, partial, 'stable', nth=1)
            check(h and 'stable' in h['contents']['value'], 'hover survives nearby syntax error', h)
            failed_format = c.rpc('textDocument/formatting', dict(textDocument=dict(uri=main_file.as_uri()), options=dict(tabSize=4, insertSpaces=True)), error=True)
            check(failed_format['code'] == -32803, 'syntax errors reject formatting edits', failed_format)
            c.change(main_file, main_text, 6); c.barrier()
            completion = c.query('completion', main_file, main_text, 'helper.sum', delta=len('helper.'))
            check(any(i['label'] == 'sum' for i in completion['items']), 'module completion', completion)
            local_completion = c.query('completion', main_file, main_text, 'helper.sum', delta=0)
            check(any(i['label'] == 'value' for i in local_completion['items']), 'local completion', local_completion)
            rename = c.query('rename', main_file, main_text, 'value', newName='renamed')
            check(sum(len(f['edits']) for f in rename['documentChanges']) == 2, 'local rename preserves shadowed binding', rename)
            # Invalid names and conflicts must produce protocol errors.
            for name in ['return', 'two words', 'sum', 'helper']:
                offset = main_text.index('value'); start = main_text.rfind('\n', 0, offset)+1
                e = c.rpc('textDocument/rename', dict(textDocument=dict(uri=main_file.as_uri()), position=dict(line=1, character=offset-start), newName=name), error=True)
                check(e['code'] == -32602, 'rename rejects '+name, e)
            features = ('struct Point { pub int x; int size(&const self) { return self.x; } }\n'
                        'trait Shape { int area(&const self); }\n'
                        'impl Shape for Point { int area(&const self) { return self.x; } }\n'
                        'int main() { var p = Point { x: 4 }; int total = p.size(); return p.area() + p.x + total; }\n')
            c.change(main_file, features, 7); c.barrier()
            check(c.diagnostics(main_file)['diagnostics'] == [], 'method and trait fixture compiles', c.diagnostics(main_file))
            h = c.query('hover', main_file, features, 'p.size', delta=2)
            check(h and 'size(' in h['contents']['value'], 'method hover', h)
            methods = c.query('completion', main_file, features, 'p.size', delta=2)
            check({'x', 'size', 'area'}.issubset({i['label'] for i in methods['items']}), 'typed member completion', methods)
            edits = c.query('rename', main_file, features, 'p.x', delta=2, newName='coordinate')
            check(sum(len(f['edits']) for f in edits['documentChanges']) == 5, 'field rename includes literal labels and method bodies', edits)
            edits = c.query('rename', main_file, features, 'p.area', delta=2, newName='surface')
            check(sum(len(f['edits']) for f in edits['documentChanges']) == 3, 'trait method rename includes declaration and implementation', edits)
            # Completion and argument hints work during unfinished expressions.
            incomplete = 'struct P { pub int amount; } int main() { var p = P { amount: 4 }; p.'
            c.change(main_file, incomplete, 8); c.barrier()
            items = c.query('completion', main_file, incomplete, 'p.', delta=2)
            check(any(i['label'] == 'amount' for i in items['items']), 'completion after an unfinished dot', items)
            incomplete = 'import helper; int main() { return helper.sum(1, '
            c.change(main_file, incomplete, 9); c.barrier()
            hint = c.query('signatureHelp', main_file, incomplete, 'sum(1, ', delta=7)
            check(hint and hint['activeParameter'] == 1, 'argument hints in unfinished call', hint)
            scope_text = 'int main() { for (index in 0..2) { print("{index}"); } int result = 0; return result; }'
            c.change(main_file, scope_text, 10); c.barrier()
            items = c.query('completion', main_file, scope_text, 'return result')
            check(not any(i['label'] == 'index' for i in items['items']), 'loop binding is absent outside its scope', items)
            closure = 'int main() { int amount = 2; var f = [&const amount] const || amount + 1; return f(); }'
            c.change(main_file, closure, 11); c.barrier()
            check(c.diagnostics(main_file)['diagnostics'] == [], 'capture fixture compiles', c.diagnostics(main_file))
            edits = c.query('rename', main_file, closure, 'amount', newName='quantity')
            check(sum(len(f['edits']) for f in edits['documentChanges']) == 3, 'rename includes explicit captures', edits)
            c.notify('textDocument/didClose', dict(textDocument=dict(uri=main_file.as_uri())))
            c.barrier()
            check(c.diagnostics(main_file)['diagnostics'] == [], 'closed document diagnostics cleared')
            # Representative malformed edits must not terminate the server.
            malformed = ['struct', 'int main(', 'import ;', 'struct S { int ; }', 'enum E { V(',
                         'int main() { match ', 'int main() { var x = [', 'trait T {', 'impl T for',
                         'int main() { unknown.', 'int main() { return Some(', 'int main() { var f = |x|']
            for i, code in enumerate(malformed):
                c.open(main_file, code, 20+i); c.barrier()
                c.notify('textDocument/didClose', dict(textDocument=dict(uri=main_file.as_uri()))); c.barrier()
            check(c.process.poll() is None, 'server survives malformed edits')
            for body in [b'{', b'{"jsonrpc":"2.0","id":1,"method":"m","id":2}',
                         br'{"jsonrpc":"2.0","id":"\ud800","method":"m"}',
                         b'{"jsonrpc":"2.0","id":01,"method":"m"}', b'\xff']:
                c.raw(body); e = c.response(None, error=True); check(e['code'] == -32700, 'malformed JSON rejected')
            c.raw(b'{')
            e = c.response(None, error=True); check(e['code'] == -32700, 'invalid JSON error')
            c.send(dict(id='unicode-\U0001f600', method='unknown/method', params={}), fragment=True)
            e = c.response('unicode-\U0001f600', error=True); check(e['code'] == -32601, 'fragmented UTF-8 frame')
            # Open a nested module first. Its sibling imports still resolve
            # against main.snx's directory.
            subdir = folder / 'nested'; subdir.mkdir()
            module = subdir / 'module.snx'
            nested_text = 'import helper; pub int value() { return helper.sum(1, 2); }'
            module.write_text(nested_text)
            root_text = 'import nested.module; int main() { return module.value(); }'
            main_file.write_text(root_text)
            c.notify('workspace/didChangeWatchedFiles', dict(changes=[dict(uri=module.as_uri(), type=1), dict(uri=main_file.as_uri(), type=2)]))
            c.open(module, nested_text); c.barrier()
            check(c.diagnostics(module)['diagnostics'] == [], 'nested module uses program entry import root', c.diagnostics(module))
            definition = c.query('definition', module, nested_text, 'sum')
            check(definition['uri'].lower() == helper.as_uri().lower(), 'definition from nested module')
            c.notify('$/cancelRequest', dict(id=999)); c.barrier()
            c.rpc('shutdown'); e = c.rpc('textDocument/hover', {}, error=True); check(e['code'] == -32600, 'queries rejected after shutdown'); c.notify('exit'); c.process.stdin.close(); check(c.process.wait(timeout=10) == 0, 'orderly shutdown')
            check(c.process.stderr.read() == b'', 'server stderr stays empty on success')
        finally:
            if c.process.poll() is None:
                error = c.close()
                if error: print(error, file=sys.stderr)
    print(f'lsp: {checks} checks passed')

if __name__ == '__main__':
    main()
