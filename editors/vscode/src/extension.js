// Synx extension entry point: compiler errors on open and save, the Run,
// Build, and Check commands, and the status bar item.
'use strict';

const childProcess = require('child_process');
const fs = require('fs');
const path = require('path');
const vscode = require('vscode');
const { parseCheckOutput, utf16Column } = require('./diagnostics');
const { findCompiler } = require('./compiler');
const { LanguageServer, registerProviders } = require('./language-server');
let languageServer;
let serverSupport;

const CHECK_TIMEOUT_MS = 60_000;
const RELEASES_URL = 'https://github.com/EdwardWare06/synx/releases';

/** @type {vscode.OutputChannel} */
let output;
/** @type {vscode.DiagnosticCollection} */
let collection;
/** @type {vscode.StatusBarItem} */
let status;

/** Compiler path and version; undefined until resolved, null when missing. */
/** @type {{ path: string, version: string } | null | undefined} */
let compiler;
/** Running checks by file. A newer check of the same file stops the older one. */
/** @type {Map<string, childProcess.ChildProcess>} */
const running = new Map();
/** For each checked file, the URIs that its last check reported diagnostics for. */
/** @type {Map<string, Set<string>>} */
const reported = new Map();

/** @param {vscode.ExtensionContext} context */
function activate(context) {
    output = vscode.window.createOutputChannel('Synx');
    collection = vscode.languages.createDiagnosticCollection('synx');
    status = vscode.window.createStatusBarItem('synx.status', vscode.StatusBarAlignment.Left, 0);
    status.name = 'Synx';
    status.command = 'synx.statusClicked';
    context.subscriptions.push(output, collection, status);

    context.subscriptions.push(
        vscode.commands.registerCommand('synx.runFile', (uri) => runTask('run', uri)),
        vscode.commands.registerCommand('synx.buildFile', (uri) => runTask('build', uri)),
        vscode.commands.registerCommand('synx.checkFile', checkCommand),
        vscode.commands.registerCommand('synx.showOutput', () => output.show()),
        vscode.commands.registerCommand('synx.statusClicked', statusClicked),
        vscode.workspace.onDidSaveTextDocument((document) => {
            if (setting('checkOnSave', true)) check(document, true);
        }),
        vscode.workspace.onDidOpenTextDocument((document) => {
            if (setting('checkOnOpen', true)) check(document);
        }),
        vscode.window.onDidChangeActiveTextEditor(() => updateStatus()),
        vscode.workspace.onDidChangeConfiguration((event) => {
            if (event.affectsConfiguration('synx.compilerPath') || event.affectsConfiguration('synx.entryPoints')) {
                languageServer?.dispose(); languageServer = undefined; serverSupport = undefined;
                compiler = undefined;
                updateStatus();
            }
        }),
        { dispose: () => running.forEach((child) => child.kill()) },
    );

    registerProviders(context, getLanguageServer, output);
    context.subscriptions.push(
        vscode.workspace.onDidChangeTextDocument(event => {
            if (event.document.languageId === 'synx' && setting('checkOnChange', true)) {
                getLanguageServer()?.changed(event.document, setting('checkDelay', 300));
            }
        }),
        vscode.workspace.onDidCloseTextDocument(document => languageServer?.close(document).catch(e => output.appendLine(e.message))),
        { dispose: () => languageServer?.dispose() },
    );
    const watcher = vscode.workspace.createFileSystemWatcher('**/*.{snx,toml,lock}');
    context.subscriptions.push(watcher);
    for (const [event, type] of [['onDidCreate', 1], ['onDidChange', 2], ['onDidDelete', 3]]) {
        context.subscriptions.push(watcher[event](uri => {
            if (languageServer) languageServer.ready.then(() => languageServer.transport.notify('workspace/didChangeWatchedFiles', { changes: [{ uri: uri.toString(), type }] })).catch(e => output.appendLine(e.message));
        }));
    }
    updateStatus();
    if (setting('checkOnOpen', true)) {
        for (const document of vscode.workspace.textDocuments) check(document);
    }
}

async function deactivate() { await languageServer?.dispose(); }

function getLanguageServer() {
    const found = getCompiler();
    if (!found) return undefined;
    if (serverSupport === undefined) {
        try {
            const probe = childProcess.execFileSync(found.path, ['lsp', '--probe'], { timeout: 10000, windowsHide: true, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
            serverSupport = probe.trim() === 'synx-lsp-1';
        } catch { serverSupport = false; }
        if (!serverSupport) output.appendLine('This compiler uses checks on saved files. Install a compiler with synx lsp for live editor features.');
    }
    if (!serverSupport) return undefined;
    if (languageServer?.transport.closed) languageServer = undefined;
    if (!languageServer) {
        languageServer = new LanguageServer(found.path, output, collection);
        languageServer.ready.catch(error => output.appendLine(error.message));
        for (const document of vscode.workspace.textDocuments) languageServer.sync(document).catch(error => output.appendLine(error.message));
    }
    return languageServer;
}

/**
 * @template T
 * @param {string} name
 * @param {T} fallback
 * @returns {T}
 */
function setting(name, fallback) {
    return vscode.workspace.getConfiguration('synx').get(name, fallback);
}

/**
 * Finds the compiler and reads its version. A missing compiler is searched
 * again on each call, so installing Synx needs no reload.
 * @returns {{ path: string, version: string } | null}
 */
function getCompiler() {
    if (compiler) return compiler;
    const configured = setting('compilerPath', '');
    const found = findCompiler(configured);
    if (!found) {
        if (compiler !== null) {
            output.appendLine(configured
                ? `Synx compiler not found at ${configured} (setting synx.compilerPath).`
                : 'Synx compiler not found in SYNX_HOME, PATH, or the default install folder.');
        }
        compiler = null;
        return null;
    }
    let version = 'Synx';
    try {
        const text = childProcess.execFileSync(found, ['version'], { timeout: 10_000, windowsHide: true, encoding: 'utf8' });
        const match = /Synx\s+(\S+)/.exec(text);
        if (match) version = `Synx ${match[1]}`;
    } catch (error) {
        output.appendLine(`${found} version failed: ${error instanceof Error ? error.message : error}`);
    }
    output.appendLine(`Using ${version} at ${found}`);
    compiler = { path: found, version };
    return compiler;
}

/** @param {boolean} [busy] */
function updateStatus(busy = false) {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document.languageId !== 'synx') {
        status.hide();
        return;
    }
    const found = getCompiler();
    if (!found) {
        status.text = '$(warning) Synx: compiler not found';
        status.tooltip = 'Select to download Synx or to set synx.compilerPath.';
        status.backgroundColor = new vscode.ThemeColor('statusBarItem.warningBackground');
    } else {
        status.text = busy ? `$(sync~spin) ${found.version}` : found.version;
        status.tooltip = `${found.path}\nSelect to show the Synx output.`;
        status.backgroundColor = undefined;
    }
    status.show();
}

async function statusClicked() {
    if (getCompiler()) {
        output.show();
        return;
    }
    const choice = await vscode.window.showWarningMessage(
        'The Synx compiler was not found. Install Synx, or set synx.compilerPath to the compiler.',
        'Download Synx', 'Set Compiler Path');
    if (choice === 'Download Synx') {
        vscode.env.openExternal(vscode.Uri.parse(RELEASES_URL));
    } else if (choice === 'Set Compiler Path') {
        vscode.commands.executeCommand('workbench.action.openSettings', 'synx.compilerPath');
    }
}

/**
 * Runs `synx check <file> --json` on the saved file and shows the result.
 * @param {vscode.TextDocument} document
 */
function check(document, saved = false) {
    if (document.languageId !== 'synx' || document.uri.scheme !== 'file') return;
    const found = getCompiler();
    if (!found) {
        updateStatus();
        return;
    }
    const server = getLanguageServer();
    if (server) { server.sync(document, saved).catch(error => output.appendLine(error.message)); return; }
    const file = document.uri.fsPath;
    running.get(file)?.kill();
    const cwd = path.dirname(file);
    const args = ['check', file, '--json'];
    const child = childProcess.execFile(found.path, args,
        { cwd, timeout: CHECK_TIMEOUT_MS, maxBuffer: 64 * 1024 * 1024, windowsHide: true },
        (error, stdout, stderr) => {
            if (running.get(file) !== child) return;
            running.delete(file);
            updateStatus();
            if (error && /** @type {any} */ (error).code === 'ENOENT') {
                output.appendLine(`Cannot start ${found.path}.`);
                compiler = undefined;
                updateStatus();
                return;
            }
            if (error && error.killed) {
                output.appendLine(`synx check stopped after ${CHECK_TIMEOUT_MS / 1000} s: ${file}`);
                return;
            }
            const result = parseCheckOutput(stdout, stderr, cwd);
            if (!result) {
                if (error) {
                    output.appendLine(`synx check failed for ${file} (exit ${error.code}):`);
                    output.appendLine((stderr || stdout).trimEnd());
                }
                publish(file, { diagnostics: [] });
                return;
            }
            if (result.error) output.appendLine(`synx check: ${result.error}: ${file}`);
            publish(file, result);
        });
    running.set(file, child);
    updateStatus(true);
}

/**
 * Replaces the diagnostics from the last check of `checked`. Diagnostics can
 * be in imported files; files that the last check reported and this check
 * does not are cleared.
 * @param {string} checked
 * @param {import('./diagnostics').CheckResult} result
 */
function publish(checked, result) {
    /** @type {Map<string, { uri: vscode.Uri, items: vscode.Diagnostic[] }>} */
    const files = new Map();
    const checkedUri = vscode.Uri.file(checked);
    files.set(checkedUri.toString(), { uri: checkedUri, items: [] });
    /** @type {Map<string, string[] | undefined>} */
    const lineCache = new Map();
    for (const item of result.diagnostics) {
        if (item.file.startsWith('<')) continue;
        const uri = vscode.Uri.file(item.file);
        const key = uri.toString();
        if (!files.has(key)) files.set(key, { uri, items: [] });
        if (!lineCache.has(key)) lineCache.set(key, readLines(uri));
        const lines = lineCache.get(key);
        const lineText = (/** @type {number} */ line) => lines?.[line - 1] ?? '';
        const startLine = item.start.line - 1;
        const startColumn = utf16Column(lineText(item.start.line), item.start.column);
        let endLine = item.end.line - 1;
        let endColumn = utf16Column(lineText(item.end.line), item.end.column);
        if (endLine < startLine || (endLine === startLine && endColumn <= startColumn)) {
            endLine = startLine;
            endColumn = Math.min(startColumn + 1, Math.max(lineText(item.start.line).length, startColumn));
        }
        const range = new vscode.Range(startLine, startColumn, endLine, endColumn);
        const diagnostic = new vscode.Diagnostic(range, item.message, severity(item.severity));
        diagnostic.source = 'synx';
        if (item.code) diagnostic.code = item.code;
        files.get(key)?.items.push(diagnostic);
    }
    for (const previous of reported.get(checked) ?? []) {
        if (!files.has(previous)) collection.delete(vscode.Uri.parse(previous));
    }
    for (const { uri, items } of files.values()) collection.set(uri, items);
    reported.set(checked, new Set(files.keys()));
}

/**
 * Lines of an open document, or of the file on disk.
 * @param {vscode.Uri} uri
 * @returns {string[] | undefined}
 */
function readLines(uri) {
    const open = vscode.workspace.textDocuments.find((document) => document.uri.toString() === uri.toString());
    const text = open ? open.getText() : readFile(uri.fsPath);
    return text === undefined ? undefined : text.split(/\r?\n/);
}

/** @param {string} file */
function readFile(file) {
    try {
        return fs.readFileSync(file, 'utf8');
    } catch {
        return undefined;
    }
}

/** @param {'error' | 'warning' | 'note'} value */
function severity(value) {
    if (value === 'warning') return vscode.DiagnosticSeverity.Warning;
    if (value === 'note') return vscode.DiagnosticSeverity.Information;
    return vscode.DiagnosticSeverity.Error;
}

/**
 * The Synx document for a command: the given URI (editor title button) or
 * the active editor. Unsaved changes are saved first.
 * @param {vscode.Uri | undefined} uri
 * @returns {Promise<vscode.TextDocument | undefined>}
 */
async function commandDocument(uri) {
    const document = uri instanceof vscode.Uri
        ? await vscode.workspace.openTextDocument(uri)
        : vscode.window.activeTextEditor?.document;
    if (!document || document.languageId !== 'synx') {
        vscode.window.showErrorMessage('Open a Synx (.snx) file first.');
        return undefined;
    }
    if (document.uri.scheme !== 'file') {
        vscode.window.showErrorMessage('Save the Synx file to disk first.');
        return undefined;
    }
    if (document.isDirty && !(await document.save())) return undefined;
    return document;
}

/** @returns {{ path: string, version: string } | undefined} */
function requireCompiler() {
    const found = getCompiler();
    if (!found) {
        updateStatus();
        statusClicked();
        return undefined;
    }
    return found;
}

async function checkCommand() {
    const document = await commandDocument(undefined);
    if (!document || !requireCompiler()) return;
    // Saving already started a check when checkOnSave is on.
    if (!running.has(document.uri.fsPath)) check(document, true);
}

/**
 * Runs `synx run` or `synx build` on a file as a task in the terminal panel.
 * The task starts the compiler directly, so no shell quoting applies.
 * @param {'run' | 'build'} command
 * @param {vscode.Uri | undefined} uri
 */
async function runTask(command, uri) {
    const document = await commandDocument(uri);
    if (!document) return;
    const found = requireCompiler();
    if (!found) return;
    const file = document.uri.fsPath;
    const args = [command, file, ...setting('buildArguments', /** @type {string[]} */ ([]))];
    const scope = vscode.workspace.getWorkspaceFolder(document.uri) ?? vscode.TaskScope.Workspace;
    const task = new vscode.Task({ type: 'synx', command, file }, scope, `${command} ${path.basename(file)}`, 'synx',
        new vscode.ProcessExecution(found.path, args, { cwd: path.dirname(file) }), []);
    task.presentationOptions = {
        reveal: vscode.TaskRevealKind.Always,
        panel: vscode.TaskPanelKind.Dedicated,
        clear: true,
        focus: command === 'run',
    };
    await vscode.tasks.executeTask(task);
}

module.exports = { activate, deactivate };
