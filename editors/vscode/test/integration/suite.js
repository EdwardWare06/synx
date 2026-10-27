// Runs inside the VS Code extension host, started by run.js.
'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vscode = require('vscode');

const BROKEN_MAIN = 'import helper;\nmain() {\n    var s = String.from("ü😀"); int y = undefined_name;\n    helper.f();\n}\n';
const BROKEN_HELPER = 'pub void f() { bool b = 5; }\n';
const FIXED_MAIN = 'import helper;\nint main() {\n    var s = String.from("ü😀");\n    helper.f();\n    return 7;\n}\n';
const FIXED_HELPER = 'pub void f() { bool b = true; }\n';

/**
 * @template T
 * @param {() => T | undefined} probe
 * @param {string} label
 * @returns {Promise<T>}
 */
async function waitFor(probe, label, timeout = 60_000) {
    const end = Date.now() + timeout;
    for (;;) {
        const value = probe();
        if (value !== undefined) return value;
        if (Date.now() > end) throw new Error(`timed out: ${label}`);
        await new Promise((resolve) => setTimeout(resolve, 100));
    }
}

/** @param {string} command */
async function runCommand(command) {
    const ended = new Promise((resolve) => {
        const listener = vscode.tasks.onDidEndTaskProcess((event) => {
            if (event.execution.task.source !== 'synx') return;
            listener.dispose();
            resolve(event.exitCode);
        });
    });
    await vscode.commands.executeCommand(command);
    return Promise.race([ended, new Promise((_, reject) => setTimeout(() => reject(new Error(`timed out: ${command}`)), 180_000))]);
}

async function run() {
    const workspace = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
    assert.ok(workspace, 'a workspace folder is open');
    const main = path.join(workspace, 'main.snx');
    const helper = path.join(workspace, 'helper.snx');
    fs.writeFileSync(main, BROKEN_MAIN);
    fs.writeFileSync(helper, BROKEN_HELPER);

    // Opening the file checks it. The error in the imported file is shown on
    // that file, and the column counts UTF-16 units after "ü😀".
    const document = await vscode.workspace.openTextDocument(main);
    await vscode.window.showTextDocument(document);
    assert.equal(document.languageId, 'synx');
    const unknown = await waitFor(() => vscode.languages.getDiagnostics(document.uri)
        .find((d) => d.message.includes('undefined_name')), 'diagnostic for undefined_name');
    const line = document.lineAt(2).text;
    const column = line.indexOf('undefined_name');
    assert.deepEqual([unknown.range.start.line, unknown.range.start.character, unknown.range.end.character],
        [2, column, column + 'undefined_name'.length]);
    assert.equal(unknown.severity, vscode.DiagnosticSeverity.Error);
    assert.equal(unknown.source, 'synx');
    assert.equal(unknown.code, 'SNX0401');
    const helperDiagnostics = await waitFor(() => {
        const items = vscode.languages.getDiagnostics(vscode.Uri.file(helper));
        return items.length > 0 ? items : undefined;
    }, 'diagnostic in helper.snx');
    assert.equal(helperDiagnostics.length, 1);
    assert.equal(helperDiagnostics[0].range.start.character, 'pub void f() { bool b = '.length);

    // Saving the fixed files clears the errors in both.
    fs.writeFileSync(helper, FIXED_HELPER);
    const edit = new vscode.WorkspaceEdit();
    edit.replace(document.uri, new vscode.Range(0, 0, document.lineCount, 0), FIXED_MAIN);
    assert.ok(await vscode.workspace.applyEdit(edit));
    assert.ok(await document.save());
    await waitFor(() => vscode.languages.getDiagnostics(document.uri).length === 0 &&
        vscode.languages.getDiagnostics(vscode.Uri.file(helper)).length === 0 ? true : undefined,
    'diagnostics cleared after the fix');

    // Current compilers provide language features and errors on unsaved text.
    const cp = require('node:child_process');
    let lsp = false;
    try { lsp = cp.execFileSync(vscode.workspace.getConfiguration('synx').get('compilerPath'), ['lsp', '--probe'], { windowsHide: true, encoding: 'utf8' }).trim() === 'synx-lsp-1'; } catch {}
    if (lsp) {
        const source = document.getText();
        const p = document.positionAt(source.indexOf('helper.f') + 'helper.'.length);
        const hovers = await vscode.commands.executeCommand('vscode.executeHoverProvider', document.uri, p);
        assert.ok(hovers.length && hovers[0].contents.some(c => c.value.includes('f(')), 'hover through the server');
        const definitions = await vscode.commands.executeCommand('vscode.executeDefinitionProvider', document.uri, p);
        assert.ok(definitions.some(d => (d.uri || d.targetUri).fsPath.toLowerCase() === helper.toLowerCase()), 'definition in imported module');
        const references = await vscode.commands.executeCommand('vscode.executeReferenceProvider', document.uri, p);
        assert.ok(references.length >= 1, 'resolved references');
        const completion = await vscode.commands.executeCommand('vscode.executeCompletionItemProvider', document.uri, p);
        assert.ok(completion.items.some(c => c.label === 'f'), 'module completion');
        const rename = await vscode.commands.executeCommand('vscode.executeDocumentRenameProvider', document.uri, p, 'renamed');
        assert.equal(rename.entries().reduce((n, [, edits]) => n + edits.length, 0), 2, 'rename across files');
        const dirty = new vscode.WorkspaceEdit();
        dirty.replace(document.uri, new vscode.Range(0, 0, document.lineCount, 0), source.replace('return 7', 'return unknown_live'));
        assert.ok(await vscode.workspace.applyEdit(dirty));
        await waitFor(() => vscode.languages.getDiagnostics(document.uri).some(d => d.message.includes('unknown_live')) ? true : undefined, 'live errors before save');
        assert.ok(document.isDirty, 'live check did not save the file');
        const restore = new vscode.WorkspaceEdit(); restore.replace(document.uri, new vscode.Range(0, 0, document.lineCount, 0), source);
        assert.ok(await vscode.workspace.applyEdit(restore)); assert.ok(await document.save());
        await waitFor(() => vscode.languages.getDiagnostics(document.uri).length === 0 ? true : undefined, 'live diagnostics cleared');
        const formatting = await vscode.commands.executeCommand('vscode.executeFormatDocumentProvider', document.uri, { tabSize: 2, insertSpaces: false });
        assert.equal(formatting.length, 1, 'formatter provides a document edit');
        const formatted = new vscode.WorkspaceEdit(); formatted.set(document.uri, formatting);
        assert.ok(await vscode.workspace.applyEdit(formatted));
        assert.ok(document.getText().includes('\n    return 7;'), 'formatter uses four spaces');
        assert.ok(document.isDirty, 'formatting did not save the file');
        const repeat = await vscode.commands.executeCommand('vscode.executeFormatDocumentProvider', document.uri, { tabSize: 4, insertSpaces: true });
        assert.equal((repeat ?? []).length, 0, 'editor formatting is idempotent');
        assert.ok(await document.save());
    }

    // Build and Run start the compiler as tasks.
    assert.equal(await runCommand('synx.buildFile'), 0, 'build exit code');
    const executable = path.join(workspace, process.platform === 'win32' ? 'main.exe' : 'main');
    assert.ok(fs.existsSync(executable), 'build wrote the executable');
    assert.equal(await runCommand('synx.runFile'), 7, 'run passes the program exit code');
    console.log('synx integration: all checks passed');
}

module.exports = { run };
