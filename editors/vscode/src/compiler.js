// Finds the Synx compiler executable. This module does not use the vscode
// API, so test/compiler.test.js runs it under plain Node.
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');

/**
 * On Windows the package has bin\synx.cmd, a launcher for libexec\synx.exe.
 * A .cmd file cannot start without a shell, so the extension starts
 * libexec\synx.exe directly; the compiler finds std and lib from its own
 * path. On Linux and macOS, bin/synx is the compiler or a link to it.
 *
 * The search order is: the synx.compilerPath setting, SYNX_HOME, PATH, then
 * the default install folder of the package installers. VS Code can hold an
 * environment from before Synx was installed, so the default folder is the
 * last fallback.
 * @param {string} configured  value of synx.compilerPath, may be empty
 * @param {NodeJS.ProcessEnv} env
 * @param {NodeJS.Platform} platform
 * @returns {string | undefined}  absolute path of an existing executable
 */
function findCompiler(configured, env = process.env, platform = process.platform) {
    const windows = platform === 'win32';
    const p = windows ? path.win32 : path.posix;
    if (configured) {
        const expanded = configured.replace(/^~(?=$|[\\/])/, os.homedir());
        return fromCandidate(p.resolve(expanded), windows, p);
    }
    if (env.SYNX_HOME) {
        const found = fromHome(env.SYNX_HOME, windows, p);
        if (found) return found;
    }
    const pathValue = env.PATH ?? env.Path ?? '';
    for (const dir of pathValue.split(windows ? ';' : ':')) {
        if (!dir) continue;
        const names = windows ? ['synx.cmd', 'synx.exe'] : ['synx'];
        for (const name of names) {
            const found = fromCandidate(p.join(dir.replace(/^"|"$/g, ''), name), windows, p);
            if (found) return found;
        }
    }
    const home = windows
        ? env.LOCALAPPDATA && p.join(env.LOCALAPPDATA, 'Programs', 'Synx')
        : p.join(os.homedir(), '.local', 'share', 'synx');
    return home ? fromHome(home, windows, p) : undefined;
}

/**
 * @param {string} home  installation root
 * @param {boolean} windows
 * @param {path.PlatformPath} p
 */
function fromHome(home, windows, p) {
    const file = windows ? p.join(home, 'libexec', 'synx.exe') : p.join(home, 'bin', 'synx');
    return isFile(file) ? file : undefined;
}

/**
 * Maps a synx.cmd launcher to the libexec\synx.exe beside it.
 * @param {string} file
 * @param {boolean} windows
 * @param {path.PlatformPath} p
 */
function fromCandidate(file, windows, p) {
    if (!isFile(file)) return undefined;
    if (windows && /\.(cmd|bat)$/i.test(file)) {
        return fromHome(p.dirname(p.dirname(file)), true, p);
    }
    return file;
}

/** @param {string} file */
function isFile(file) {
    try {
        return fs.statSync(file).isFile();
    } catch {
        return false;
    }
}

module.exports = { findCompiler };
