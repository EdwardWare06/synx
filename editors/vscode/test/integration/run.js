// Starts VS Code with the extension and runs suite.js in it.
// Usage: SYNX_TEST_COMPILER=<synx executable> node test/integration/run.js
// The compiler must find its standard library and runtime: an installed
// package does, and a build-tree compiler needs SYNX_STD_PATH and
// SYNX_RUNTIME_SHARED in the environment.
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { runTests } = require('@vscode/test-electron');

async function main() {
    const compiler = process.env.SYNX_TEST_COMPILER;
    if (!compiler || !fs.existsSync(compiler)) {
        console.error('Set SYNX_TEST_COMPILER to the synx executable.');
        process.exit(2);
    }
    const workspace = fs.mkdtempSync(path.join(os.tmpdir(), 'synx-vscode-test-'));
    try {
        fs.mkdirSync(path.join(workspace, '.vscode'));
        fs.writeFileSync(path.join(workspace, '.vscode', 'settings.json'),
            JSON.stringify({ 'synx.compilerPath': path.resolve(compiler) }, null, 2));
        const vscodeExecutablePath = process.env.SYNX_TEST_VSCODE;
        await runTests({
            ...(vscodeExecutablePath ? { vscodeExecutablePath } : {}),
            extensionDevelopmentPath: path.resolve(__dirname, '..', '..'),
            extensionTestsPath: path.resolve(__dirname, 'suite.js'),
            launchArgs: [workspace, '--disable-extensions', '--disable-workspace-trust'],
        });
    } finally {
        fs.rmSync(workspace, { recursive: true, force: true });
    }
}

main().catch((error) => {
    console.error(error);
    process.exit(1);
});
