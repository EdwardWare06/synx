// Tokenizes Synx samples with the TextMate grammar and checks the scopes.
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const oniguruma = require('vscode-oniguruma');
const textmate = require('vscode-textmate');

const root = path.join(__dirname, '..');

async function loadGrammar() {
    const wasm = fs.readFileSync(require.resolve('vscode-oniguruma/release/onig.wasm'));
    await oniguruma.loadWASM(wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.byteLength));
    const registry = new textmate.Registry({
        onigLib: Promise.resolve({
            createOnigScanner: (patterns) => new oniguruma.OnigScanner(patterns),
            createOnigString: (text) => new oniguruma.OnigString(text),
        }),
        loadGrammar: async () => {
            const file = path.join(root, 'syntaxes', 'synx.tmLanguage.json');
            return textmate.parseRawGrammar(fs.readFileSync(file, 'utf8'), file);
        },
    });
    return registry.loadGrammar('source.synx');
}

/** Returns [text, innermost scope] for each token of the lines. */
function tokenize(grammar, source) {
    let state = textmate.INITIAL;
    const tokens = [];
    for (const line of source.split('\n')) {
        const result = grammar.tokenizeLine(line, state);
        for (const token of result.tokens) {
            const text = line.slice(token.startIndex, token.endIndex);
            if (text.trim()) tokens.push([text.trim(), token.scopes[token.scopes.length - 1], token.scopes]);
        }
        state = result.ruleStack;
    }
    return tokens;
}

function scopeOf(tokens, text) {
    const token = tokens.find(([t]) => t === text);
    assert.ok(token, `no token ${text}`);
    return token[1];
}

test('grammar scopes', async () => {
    const grammar = await loadGrammar();
    const tokens = tokenize(grammar, [
        '/// Doc comment.',
        'import std.math.sqrt;',
        'pub struct Point { pub double x; }',
        '/* outer /* inner */ still */ int after;',
        '&const T bigger<T: Shape>(&const T a) {',
        '    var s = "sum {a.len() + 1} \\n\\{";',
        '    var braces = "{{ name }}";',
        '    char c = \'\\u{3bb}\';',
        '    long big = 0xFF_FFL; float f = 1.5f; double d = 2.5e3;',
        '    for (i in 0..=10) { if (x != 0) { return Option.Some(i); } }',
        '    match t { Token.Plus => self.go(), _ => {} }',
        '}',
    ].join('\n'));
    assert.equal(scopeOf(tokens, '/// Doc comment.'), 'comment.line.documentation.synx');
    assert.equal(scopeOf(tokens, 'import'), 'keyword.control.import.synx');
    assert.equal(scopeOf(tokens, 'std.math.sqrt'), 'entity.name.namespace.synx');
    assert.equal(scopeOf(tokens, 'struct'), 'storage.type.struct.synx');
    assert.equal(scopeOf(tokens, 'Point'), 'entity.name.type.synx');
    assert.equal(scopeOf(tokens, 'double'), 'storage.type.primitive.synx');
    assert.equal(scopeOf(tokens, 'still'), 'comment.block.synx');
    assert.equal(scopeOf(tokens, 'after'), 'source.synx');
    assert.equal(scopeOf(tokens, 'bigger'), 'entity.name.function.call.synx');
    assert.equal(scopeOf(tokens, 'const'), 'storage.modifier.synx');
    assert.equal(scopeOf(tokens, 'len'), 'entity.name.function.call.synx');
    assert.equal(scopeOf(tokens, '\\n'), 'constant.character.escape.synx');
    assert.equal(scopeOf(tokens, '\\{'), 'constant.character.escape.synx');
    assert.equal(scopeOf(tokens, '\\u{3bb}'), 'constant.character.escape.unicode.synx');
    assert.equal(scopeOf(tokens, '0xFF_FFL'), 'constant.numeric.hex.synx');
    assert.equal(scopeOf(tokens, '1.5f'), 'constant.numeric.float.synx');
    assert.equal(scopeOf(tokens, '2.5e3'), 'constant.numeric.float.synx');
    assert.equal(scopeOf(tokens, '..='), 'keyword.operator.range.synx');
    assert.equal(scopeOf(tokens, 'return'), 'keyword.control.synx');
    assert.equal(scopeOf(tokens, 'Option'), 'support.type.synx');
    assert.equal(scopeOf(tokens, '=>'), 'keyword.operator.arrow.synx');
    assert.equal(scopeOf(tokens, 'self'), 'variable.language.self.synx');
    // The interpolation holds code, and the string continues after it.
    const plus = tokens.find(([t, , scopes]) => t === '+' && scopes.includes('meta.interpolation.synx'));
    assert.ok(plus, 'operator inside the interpolation');
    assert.equal(plus[1], 'keyword.operator.synx');
});

test('repository sources tokenize without a scope that runs to the end', async () => {
    const grammar = await loadGrammar();
    const repo = path.join(root, '..', '..');
    const files = ['examples', 'std', 'selfhost']
        .flatMap((dir) => fs.readdirSync(path.join(repo, dir), { recursive: true }).map((n) => path.join(dir, String(n))))
        .filter((name) => name.endsWith('.snx'));
    assert.ok(files.length > 50, 'found the repository sources');
    for (const name of files) {
        const source = fs.readFileSync(path.join(repo, name), 'utf8').replace(/\r\n/g, '\n');
        let state = textmate.INITIAL;
        for (const line of source.split('\n')) state = grammar.tokenizeLine(line, state).ruleStack;
        // Only the root rule remains when every string, comment, and block closed.
        assert.equal(state.depth, 1, `${name} leaves an open string, comment, or block`);
    }
});
