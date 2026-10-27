// Unit tests for the parts that do not need VS Code: node --test test/
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { parseCheckOutput, utf16Column } = require('../src/diagnostics');
const { findCompiler } = require('../src/compiler');

const cwd = path.resolve('/work');

test('JSON output gives absolute files, codes, and ranges', () => {
    const stdout = JSON.stringify({
        version: 1, ok: false, diagnostics: [
            { file: 'main.snx', severity: 'error', code: 'SNX0402', message: 'type "x"',
              range: { start: { line: 3, column: 13 }, end: { line: 3, column: 27 } } },
            { file: '<prelude>', severity: 'note', code: '', message: 'n',
              range: { start: { line: 1, column: 1 }, end: { line: 1, column: 1 } } },
        ],
    });
    const result = parseCheckOutput(stdout, '', cwd);
    assert.ok(result);
    assert.equal(result.diagnostics.length, 2);
    assert.deepEqual(result.diagnostics[0], {
        file: path.join(cwd, 'main.snx'), severity: 'error', code: 'SNX0402', message: 'type "x"',
        start: { line: 3, column: 13 }, end: { line: 3, column: 27 },
    });
    assert.equal(result.diagnostics[1].file, '<prelude>');
    assert.equal(result.diagnostics[1].severity, 'note');
});

test('JSON output with an error and no diagnostics', () => {
    const result = parseCheckOutput('{"version":1,"ok":false,"error":"cannot read file","diagnostics":[]}\n', '', cwd);
    assert.deepEqual(result, { diagnostics: [], error: 'cannot read file' });
});

test('text output from compilers without --json', () => {
    const stderr = [
        'C:/proj/main.snx:4:40: error[SNX0401]: unknown name \'undefined_name\'',
        '4 |     var s = String.from("ü"); int y = undefined_name;',
        '  |                                        ^^^^^^^^^^^^^^',
        'lib/helper.snx:1:25: error: expected an expression, found ;',
        '1 | pub void f() { bool b = ; }',
        '  |                         ^',
        '2 errors',
    ].join('\r\n');
    const result = parseCheckOutput('', stderr, cwd);
    assert.ok(result);
    assert.equal(result.diagnostics.length, 2);
    assert.equal(result.diagnostics[0].file, path.resolve(cwd, 'C:/proj/main.snx'));
    assert.equal(result.diagnostics[0].code, 'SNX0401');
    assert.deepEqual(result.diagnostics[0].end, { line: 4, column: 54 });
    assert.equal(result.diagnostics[1].file, path.join(cwd, 'lib', 'helper.snx'));
    assert.equal(result.diagnostics[1].code, '');
    assert.deepEqual(result.diagnostics[1].end, { line: 1, column: 26 });
});

test('unrecognized output gives undefined', () => {
    assert.equal(parseCheckOutput('', 'internal error: the prelude does not check\n', cwd), undefined);
    assert.equal(parseCheckOutput('{not json', '', cwd), undefined);
});

test('byte columns convert to UTF-16 columns', () => {
    assert.equal(utf16Column('abc', 1), 0);
    assert.equal(utf16Column('abc', 3), 2);
    assert.equal(utf16Column('abc', 99), 3);
    // ü is 2 bytes and 1 UTF-16 unit; 😀 is 4 bytes and 2 units.
    const line = 'x = "ü😀"; y';
    const yByte = Buffer.from(line.slice(0, line.indexOf('y')), 'utf8').length + 1;
    assert.equal(utf16Column(line, yByte), line.indexOf('y'));
    // A column inside a multi-byte character stops before it.
    assert.equal(utf16Column('ü', 2), 0);
});

test('compiler search: setting, SYNX_HOME, PATH, and .cmd launchers', () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'synx-vscode-'));
    try {
        const windows = process.platform === 'win32';
        const compiler = windows ? path.join(root, 'libexec', 'synx.exe') : path.join(root, 'bin', 'synx');
        fs.mkdirSync(path.dirname(compiler), { recursive: true });
        fs.writeFileSync(compiler, '');
        const env = { PATH: '', LOCALAPPDATA: path.join(root, 'none') };
        assert.equal(findCompiler(compiler, env), compiler);
        assert.equal(findCompiler(path.join(root, 'missing'), env), undefined);
        assert.equal(findCompiler('', { ...env, SYNX_HOME: root }), compiler);
        if (windows) {
            fs.mkdirSync(path.join(root, 'bin'));
            fs.writeFileSync(path.join(root, 'bin', 'synx.cmd'), '');
            assert.equal(findCompiler(path.join(root, 'bin', 'synx.cmd'), env), compiler);
            assert.equal(findCompiler('', { ...env, PATH: `C:\\nowhere;${path.join(root, 'bin')}` }), compiler);
        } else {
            assert.equal(findCompiler('', { ...env, PATH: `/nowhere:${path.join(root, 'bin')}` }), compiler);
        }
    } finally {
        fs.rmSync(root, { recursive: true, force: true });
    }
});
