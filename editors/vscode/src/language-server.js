// Editor providers backed by the self-hosted `synx lsp` process.
'use strict';
const childProcess = require('node:child_process');
const vscode = require('vscode');
const fs = require('node:fs');
const path = require('node:path');
function fileKey(value) {
    try { value = fs.realpathSync.native(value); } catch { value = path.resolve(value); }
    return process.platform === 'win32' ? value.toLowerCase() : value;
}
const { Transport } = require('./lsp-transport');
const selector = { language: 'synx', scheme: 'file' };
const asPosition = p => ({ line: p.line, character: p.character });
const asRange = r => new vscode.Range(r.start.line, r.start.character, r.end.line, r.end.character);
const location = l => new vscode.Location(vscode.Uri.parse(l.uri), asRange(l.range));
class LanguageServer {
    constructor(compiler, output, diagnostics) {
        this.output = output;
        this.diagnostics = diagnostics;
        this.versions = new Map();
        this.uriAliases = new Map();
        this.timers = new Map();
        const child = childProcess.spawn(compiler, ['lsp'], { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
        child.stderr.on('data', data => output.append(data.toString('utf8')));
        this.transport = new Transport(child);
        this.transport.on('notification', (method, params) => this.notification(method, params));
        this.transport.on('closed', error => { output.appendLine(error.message); this.versions.clear(); });
        this.ready = this.initialize();
    }
    async initialize() {
        this.output.appendLine('Starting Synx language server.');
        const files = await vscode.workspace.findFiles('**/*.snx', '**/{.git,node_modules,build,build-*,dist}/**');
        for (const uri of files) this.uriAliases.set(fileKey(uri.fsPath), uri);
        await this.transport.request('initialize', {
            processId: process.pid,
            clientInfo: { name: 'Synx VS Code', version: '0.2.0' },
            rootUri: vscode.workspace.workspaceFolders?.[0]?.uri.toString() ?? null,
            workspaceFolders: vscode.workspace.workspaceFolders?.map(f => ({ uri: f.uri.toString(), name: f.name })) ?? null,
            capabilities: { general: { positionEncodings: ['utf-16'] }, workspace: { workspaceEdit: { documentChanges: true } } },
            initializationOptions: {
                files: files.map(f => f.toString()).sort(),
                entryPoints: (vscode.workspace.getConfiguration('synx').get('entryPoints', [])).flatMap(entry => {
                    if (path.isAbsolute(entry)) return [vscode.Uri.file(entry).toString()];
                    return (vscode.workspace.workspaceFolders ?? []).map(folder => vscode.Uri.joinPath(folder.uri, entry).toString());
                }),
            },
        });
        this.transport.notify('initialized', {});
        this.output.appendLine('Synx language server ready.');
    }
    async sync(document, saved = false) {
        if (document.languageId !== 'synx' || document.uri.scheme !== 'file') return;
        await this.ready;
        const uri = document.uri.toString();
        this.uriAliases.set(fileKey(document.uri.fsPath), document.uri);
        clearTimeout(this.timers.get(uri));
        this.timers.delete(uri);
        const version = this.versions.get(uri);
        if (version === undefined) this.transport.notify('textDocument/didOpen', {
            textDocument: { uri, languageId: 'synx', version: document.version, text: document.getText() },
        });
        else if (version !== document.version) this.transport.notify('textDocument/didChange', {
            textDocument: { uri, version: document.version }, contentChanges: [{ text: document.getText() }],
        });
        this.versions.set(uri, document.version);
        if (saved) this.transport.notify('textDocument/didSave', { textDocument: { uri } });
    }
    changed(document, delay) {
        const uri = document.uri.toString();
        clearTimeout(this.timers.get(uri));
        this.timers.set(uri, setTimeout(() => this.sync(document).catch(e => this.output.appendLine(e.message)), delay));
    }
    async close(document) {
        const uri = document.uri.toString();
        clearTimeout(this.timers.get(uri)); this.timers.delete(uri);
        await this.ready;
        if (this.versions.delete(uri)) this.transport.notify('textDocument/didClose', { textDocument: { uri } });
    }
    preferredUri(value) {
        const uri = vscode.Uri.parse(value);
        return uri.scheme === 'file' ? (this.uriAliases.get(fileKey(uri.fsPath)) ?? uri) : uri;
    }
    remap(value) {
        if (Array.isArray(value)) { for (const item of value) this.remap(item); }
        else if (value && typeof value === 'object') {
            if (typeof value.uri === 'string') value.uri = this.preferredUri(value.uri).toString();
            for (const item of Object.values(value)) this.remap(item);
        }
        return value;
    }
    notification(method, params) {
        if (method !== 'textDocument/publishDiagnostics') return;
        const uri = this.preferredUri(params.uri);
        const open = vscode.workspace.textDocuments.find(d => d.uri.toString() === uri.toString() || (process.platform === 'win32' ? d.uri.fsPath.toLowerCase() === uri.fsPath.toLowerCase() : d.uri.fsPath === uri.fsPath));
        if (open && params.version !== undefined && open.version !== params.version) return;
        this.diagnostics.set(open?.uri ?? uri, params.diagnostics.map(item => {
            const d = new vscode.Diagnostic(asRange(item.range), item.message,
                item.severity === 2 ? vscode.DiagnosticSeverity.Warning : item.severity === 3 ? vscode.DiagnosticSeverity.Information : vscode.DiagnosticSeverity.Error);
            d.source = 'synx'; d.code = item.code; return d;
        }));
    }
    async query(method, document, position, token, extra = {}) {
        await this.sync(document);
        const version = document.version;
        const params = { textDocument: { uri: document.uri.toString() }, ...extra };
        if (position) params.position = asPosition(position);
        const result = await this.transport.request(method, params, token);
        if (document.version !== version) return null;
        return this.remap(result);
    }
    async dispose() {
        for (const timer of this.timers.values()) clearTimeout(timer);
        this.timers.clear();
        await this.transport.stop();
    }
}
function registerProviders(context, getServer, output) {
    const invoke = async (method, document, position, token, extra) => {
        try { const server = getServer(); return server ? await server.query(method, document, position, token, extra) : null; }
        catch (error) { if (!token?.isCancellationRequested) output.appendLine(error.message); return null; }
    };
    context.subscriptions.push(
        vscode.languages.registerDocumentFormattingEditProvider(selector, { async provideDocumentFormattingEdits(d, options, t) {
            const r = await invoke('textDocument/formatting', d, null, t, { options });
            return r?.map(e => vscode.TextEdit.replace(asRange(e.range), e.newText));
        } }),
        vscode.languages.registerHoverProvider(selector, { async provideHover(d, p, t) {
            const r = await invoke('textDocument/hover', d, p, t); return r ? new vscode.Hover(new vscode.MarkdownString(r.contents.value), asRange(r.range)) : undefined;
        } }),
        vscode.languages.registerDefinitionProvider(selector, { async provideDefinition(d, p, t) {
            const r = await invoke('textDocument/definition', d, p, t); return r ? (Array.isArray(r) ? r.map(location) : location(r)) : undefined;
        } }),
        vscode.languages.registerReferenceProvider(selector, { async provideReferences(d, p, context, t) {
            const r = await invoke('textDocument/references', d, p, t, { context }); return r?.map(location);
        } }),
        vscode.languages.registerCompletionItemProvider(selector, { async provideCompletionItems(d, p, t) {
            const r = await invoke('textDocument/completion', d, p, t);
            return r ? new vscode.CompletionList(r.items.map(item => {
                const c = new vscode.CompletionItem(item.label, item.kind - 1); c.detail = item.detail; c.documentation = item.documentation; return c;
            }), r.isIncomplete) : undefined;
        } }, '.'),
        vscode.languages.registerSignatureHelpProvider(selector, { async provideSignatureHelp(d, p, t) {
            const r = await invoke('textDocument/signatureHelp', d, p, t); if (!r) return;
            const help = new vscode.SignatureHelp(); help.activeSignature = r.activeSignature; help.activeParameter = r.activeParameter;
            help.signatures = r.signatures.map(s => { const signature = new vscode.SignatureInformation(s.label, s.documentation); signature.parameters = s.parameters.map(p => new vscode.ParameterInformation(p.label)); return signature; }); return help;
        } }, '(', ','),
        vscode.languages.registerRenameProvider(selector, {
            async prepareRename(d, p, t) { const r = await invoke('textDocument/prepareRename', d, p, t); if (!r) throw new Error('This symbol cannot be renamed.'); return asRange(r); },
            async provideRenameEdits(d, p, newName, t) {
                const server = getServer(); if (!server) return;
                const r = await server.query('textDocument/rename', d, p, t, { newName }); if (!r) return;
                const edit = new vscode.WorkspaceEdit();
                for (const change of r.documentChanges) {
                    const uri = vscode.Uri.parse(change.textDocument.uri);
                    const open = vscode.workspace.textDocuments.find(doc => (process.platform === 'win32' ? doc.uri.fsPath.toLowerCase() === uri.fsPath.toLowerCase() : doc.uri.fsPath === uri.fsPath));
                    if (open && change.textDocument.version !== null && open.version !== change.textDocument.version) throw new Error('Document changed during rename. Retry the rename.');
                    edit.set(open?.uri ?? uri, change.edits.map(e => vscode.TextEdit.replace(asRange(e.range), e.newText)));
                } return edit;
            },
        }),
    );
}
module.exports = { LanguageServer, registerProviders };
