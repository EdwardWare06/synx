"""Command-line checks for the Synx compiler driver.

Usage: python tests/cli/test_cli.py SYNX RUNTIME_DIR [--update]

SYNX is the compiler under test. RUNTIME_DIR holds the shared runtime
(synx_rt.dll, libsynx_rt.so, or libsynx_rt.dylib) for the relocated-package
check. --update rewrites the snapshots in tests/cli/snapshots from SYNX.

Checks:
- driver core: selfhost/driver_probe.snx built in all four modes reports the
  embedded prelude and the fixed output-path, link-command, and option-parsing
  records in tests/cli/driver_core.bin;
- command output: usage errors, lex, parse, check, ir, and modules on a
  Unicode file name and on malformed input match the snapshots;
- about: the credit and reference lines;
- build and run in all four modes: exit status, output, deterministic objects,
  and object removal without --keep-obj;
- a relocated package with spaces and Unicode in its path builds a program
  whose output name holds shell characters, with no environment overrides.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
HERE = Path(__file__).resolve().parent
SNAPSHOTS = HERE / "snapshots"
EXE = ".exe" if sys.platform == "win32" else ""
OBJ = ".obj" if sys.platform == "win32" else ".o"
MODES = [[], ["--no-regalloc"], ["--release"], ["--no-regalloc", "--release"]]
ENV_KEYS = ["SYNX_STD_PATH", "SYNX_RUNTIME_LIB", "SYNX_RUNTIME_SHARED", "SYNX_LINKER", "SYNX_LINKER_DRIVER"]
checks = failures = 0


def check(condition, label, detail=""):
    global checks, failures
    checks += 1
    if not condition:
        failures += 1
        print(f"FAIL {label}" + (f"\n{detail}" if detail else ""))


def run(program, args, cwd, env=None):
    result = subprocess.run([str(program), *args], cwd=cwd, capture_output=True, env=env, timeout=900)
    text = lambda data: data.decode("utf-8", "replace").replace("\r\n", "\n")
    return result.returncode, text(result.stdout), text(result.stderr)


def snapshot_text(code, out, err):
    text = f"exit {code}\n--- stdout\n{out}--- stderr\n{err}"
    for host in ("windows", "linux", "macos"):
        text = text.replace(f"({host})", "(<host>)")
    return text


def read_blob(data, at):
    size = int.from_bytes(data[at:at + 8], "little")
    return data[at + 8:at + 8 + size], at + 8 + size


def main():
    args = sys.argv[1:]
    update = "--update" in args
    if update:
        args.remove("--update")
    if len(args) != 2:
        sys.exit(__doc__)
    synx, runtime_dir = Path(args[0]).resolve(), Path(args[1]).resolve()
    env = dict(os.environ)
    for key in ENV_KEYS:
        env.pop(key, None)
    env["SYNX_STD_PATH"] = str(ROOT / "std")
    work = Path(tempfile.mkdtemp(prefix="synx-cli-"))
    try:
        driver_core(synx, work, env, update)
        commands(synx, work, env, update)
        about(synx, work, env)
        builds(synx, work, env)
        relocated(synx, runtime_dir, work)
    finally:
        if failures == 0:
            shutil.rmtree(work, ignore_errors=True)
        else:
            print(f"artifacts: {work}")
    print(f"cli: {checks} checks, {failures} failures")
    sys.exit(1 if failures else 0)


def driver_core(synx, work, env, update):
    expected_path = HERE / "driver_core.bin"
    prelude = (ROOT / "selfhost" / "prelude.snx").read_bytes()
    for mode, flags in enumerate(MODES):
        probe = work / f"driver-probe-{mode}{EXE}"
        code, out, err = run(synx, ["build", str(ROOT / "selfhost" / "driver_probe.snx"), "-o", str(probe), *flags], work, env)
        check(code == 0, f"driver core {mode}: probe builds", out + err)
        if code:
            continue
        report = work / f"driver-core-{mode}.bin"
        code, out, err = run(probe, [str(report)], work, env)
        check(code == 0, f"driver core {mode}: probe runs", out + err)
        data = report.read_bytes()
        embedded, end = read_blob(data, 0)
        rest = data[end:]
        check(embedded == prelude, f"driver core {mode}: the embedded prelude matches selfhost/prelude.snx")
        if update and mode == 0:
            expected_path.write_bytes(rest)
        check(rest == expected_path.read_bytes(), f"driver core {mode}: records match driver_core.bin")


def commands(synx, work, env, update):
    entry = "entry café λ.snx"
    (work / entry).write_bytes(("import std.io; struct Item { int value; } int main() { double x = 1.234567890123; "
                                "char c = '\\u{3bb}'; io.print(\"driver café\"); return 7; }\n").encode("utf-8"))
    (work / "broken.snx").write_bytes(b"int main() { int x = true; return x; }\n")
    (work / "malformed.snx").write_bytes(b"struct Broken { int value;\nmain( {\n")
    (work / "bytes.snx").write_bytes(b'main(){ var x = "a\\0b"; var \xff = 1; }\n')
    cases = [[], ["version"], ["--version"], ["unknown"], ["check"], ["build"], ["build", "-o"], ["build", "--bad"],
             ["check", "missing.snx"], ["lex", entry], ["parse", entry], ["parse", entry, "--strict"],
             ["lex", "bytes.snx"], ["parse", "bytes.snx", "--strict"], ["parse", "malformed.snx", "--strict"],
             ["check", entry], ["check", "broken.snx"], ["ir", entry], ["ir", "broken.snx"]]
    cases += [["modules", entry, *([flag] if flag else [])]
              for flag in ("", "--signatures", "--check", "--lower", "--borrow", "--async", "--machine")]
    SNAPSHOTS.mkdir(exist_ok=True)
    for number, case in enumerate(cases):
        name = "-".join([f"{number:02d}", *[a.replace(entry, "entry").lstrip("-").replace(".snx", "") for a in case]]) or f"{number:02d}"
        code, out, err = run(synx, case, work, env)
        if case[:1] in (["ir"], ["modules"]) and out:
            # These dumps include the whole prelude and standard library; the
            # fixtures check their content. Compare status and errors only.
            check("main" in out, f"command {' '.join(case)} prints the program", out[:400])
            out = "(dump not compared)\n"
        got = snapshot_text(code, out, err)
        path = SNAPSHOTS / f"{name}.txt"
        if update:
            path.write_bytes(got.encode("utf-8"))
        want = path.read_bytes().decode("utf-8") if path.exists() else "(missing snapshot)"
        check(got == want, f"command {' '.join(case) or '(none)'} matches {path.name}", got if got != want else "")


def about(synx, work, env):
    code, out, err = run(synx, ["about"], work, env)
    check(code == 0 and err == "", "about: exits 0 without errors", err)
    check("Created by Edward Ware" in out, "about: credit line")
    check("This is not an Arasaka product choom" in out, "about: reference line")


def builds(synx, work, env):
    entry = work / "entry café λ.snx"
    program = work / f"program{EXE}"
    expected = "driver café\n"
    for mode, flags in enumerate(MODES):
        objects = []
        for attempt in range(2):
            code, out, err = run(synx, ["build", str(entry), "--keep-obj", "-o", str(program), *flags], work, env)
            check(code == 0, f"build {mode}: builds", out + err)
            objects.append(Path(str(program) + OBJ).read_bytes() if code == 0 else b"")
        check(objects[0] == objects[1] and objects[0], f"build {mode}: objects are deterministic")
        code, out, err = run(program, [], work, env)
        check(code == 7 and out == expected and err == "", f"build {mode}: program exits 7 with its output", out + err)
    Path(str(program) + OBJ).unlink()
    code, out, err = run(synx, ["run", str(entry), "-o", str(program)], work, env)
    check(code == 7 and out.endswith(expected), "run: passes the exit status and output through", out + err)
    check(not Path(str(program) + OBJ).exists(), "run: removes the object without --keep-obj")


def relocated(synx, runtime_dir, work):
    capsule = {"win32": "synx_rt.dll", "darwin": "libsynx_rt.dylib"}.get(sys.platform, "libsynx_rt.so")
    package = work / "installed café λ"
    (package / "bin").mkdir(parents=True)
    (package / "lib").mkdir()
    installed = package / "bin" / f"synx{EXE}"
    shutil.copy2(synx, installed)
    shutil.copy2(runtime_dir / capsule, package / "bin" / capsule)
    shutil.copy2(runtime_dir / capsule, package / "lib" / capsule)
    shutil.copytree(ROOT / "std", package / "std")
    env = {k: v for k, v in os.environ.items() if k not in ENV_KEYS}
    literal = work / f"literal & '$(name) λ{EXE}"
    code, out, err = run(installed, ["build", str(work / "entry café λ.snx"), "-o", str(literal)], work, env)
    check(code == 0, "relocated package: builds without environment overrides", out + err)
    if code == 0:
        code, out, err = run(literal, [], work, env)
        check(code == 7 and out == "driver café\n", "relocated package: the program runs", out + err)


if __name__ == "__main__":
    main()
