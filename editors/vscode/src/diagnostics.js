// Parses the output of `synx check`. This module does not use the vscode API,
// so test/diagnostics.test.js runs it under plain Node.
'use strict';

const path = require('path');

/**
 * @typedef {{ line: number, column: number }} Position
 *   One-based line and one-based UTF-8 byte column, as the compiler writes them.
 * @typedef {{ file: string, severity: 'error' | 'warning' | 'note', code: string,
 *             message: string, start: Position, end: Position }} CompilerDiagnostic
 *   The end is exclusive. `file` is absolute.
 * @typedef {{ diagnostics: CompilerDiagnostic[], error?: string }} CheckResult
 */

/**
 * Parses `synx check <file> --json` output. Compilers older than 0.2 ignore
 * --json and write text diagnostics to stderr, so text is parsed when stdout
 * does not hold JSON.
 * @param {string} stdout
 * @param {string} stderr
 * @param {string} cwd  folder the compiler ran in; relative paths resolve from it
 * @returns {CheckResult | undefined}  undefined when neither form is found
 */
function parseCheckOutput(stdout, stderr, cwd) {
    const trimmed = stdout.trim();
    if (trimmed.startsWith('{')) {
        const json = parseJson(trimmed, cwd);
        if (json) return json;
    }
    const text = parseText(stderr, cwd);
    return text.length > 0 ? { diagnostics: text } : undefined;
}

/**
 * @param {string} text
 * @param {string} cwd
 * @returns {CheckResult | undefined}
 */
function parseJson(text, cwd) {
    let value;
    try {
        value = JSON.parse(text);
    } catch {
        return undefined;
    }
    if (!value || !Array.isArray(value.diagnostics)) return undefined;
    /** @type {CompilerDiagnostic[]} */
    const diagnostics = [];
    for (const item of value.diagnostics) {
        if (!item || typeof item.file !== 'string' || !item.range) continue;
        diagnostics.push({
            file: resolveFile(item.file, cwd),
            severity: severityOf(item.severity),
            code: typeof item.code === 'string' ? item.code : '',
            message: String(item.message ?? ''),
            start: positionOf(item.range.start),
            end: positionOf(item.range.end),
        });
    }
    /** @type {CheckResult} */
    const result = { diagnostics };
    if (typeof value.error === 'string') result.error = value.error;
    return result;
}

// path:line:col: severity[CODE]: message, then the source line and a caret
// line whose ^ count is the span width in bytes.
const HEADER = /^(.+?):(\d+):(\d+): (error|warning|note)(?:\[([A-Za-z0-9]+)\])?: (.*)$/;
const CARET = /^\s*\|\s*?([ \t]*)(\^+)\s*$/;

/**
 * @param {string} stderr
 * @param {string} cwd
 * @returns {CompilerDiagnostic[]}
 */
function parseText(stderr, cwd) {
    const lines = stderr.split(/\r?\n/);
    /** @type {CompilerDiagnostic[]} */
    const diagnostics = [];
    for (let i = 0; i < lines.length; i++) {
        const header = HEADER.exec(lines[i]);
        if (!header) continue;
        const line = Number(header[2]);
        const column = Number(header[3]);
        let width = 1;
        const caret = i + 2 < lines.length ? CARET.exec(lines[i + 2]) : null;
        if (caret) width = caret[2].length;
        diagnostics.push({
            file: resolveFile(header[1], cwd),
            severity: severityOf(header[4]),
            code: header[5] ?? '',
            message: header[6],
            start: { line, column },
            end: { line, column: column + width },
        });
    }
    return diagnostics;
}

/** @param {unknown} value */
function severityOf(value) {
    return value === 'warning' || value === 'note' ? value : 'error';
}

/** @param {any} value @returns {Position} */
function positionOf(value) {
    const line = Number(value?.line);
    const column = Number(value?.column);
    return {
        line: Number.isInteger(line) && line > 0 ? line : 1,
        column: Number.isInteger(column) && column > 0 ? column : 1,
    };
}

/**
 * Diagnostics in the embedded prelude have the name <prelude>; they are kept
 * as is, and the caller skips them.
 * @param {string} file
 * @param {string} cwd
 */
function resolveFile(file, cwd) {
    if (file.startsWith('<')) return file;
    return path.resolve(cwd, file);
}

/**
 * Converts a one-based UTF-8 byte column to a zero-based UTF-16 column, the
 * unit VS Code positions use. A column past the end of the line clamps to it.
 * @param {string} lineText
 * @param {number} byteColumn
 */
function utf16Column(lineText, byteColumn) {
    const bytes = Buffer.from(lineText, 'utf8');
    const count = Math.min(Math.max(byteColumn - 1, 0), bytes.length);
    let column = 0;
    let offset = 0;
    for (const ch of lineText) {
        const size = Buffer.byteLength(ch, 'utf8');
        if (offset + size > count) break;
        offset += size;
        column += ch.length;
    }
    return column;
}

module.exports = { parseCheckOutput, utf16Column };
