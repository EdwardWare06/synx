"""Runs Synx fixture files through a Synx compiler.

Usage: python tests/fixtures/run.py SYNX DIR... [--filter TEXT] [--update]

A fixture is a .snx file in DIR (searched recursively) that starts with:

    // fixture: <label>
    // stage: <stage>
    // expect: <expectation>
    // expect: <expectation>      (more lines are allowed)

Expectations:

    ok                       no errors (stage check: through type checking;
                             other stages: synx check, all passes)
    error <text>             synx check reports an error containing <text>
    errors <count>           synx check reports exactly <count> errors
    type <fn> <type>         the checked return type of <fn> is <type>
    parse has <text>         synx parse succeeds and its dump contains <text>
    parse errors <count>     synx parse reports exactly <count> errors
    ir has|lacks <text>      synx ir succeeds and the IR contains / lacks <text>
    ir <fn> has|lacks <text> the same, within the IR of function <fn> only
    output <args>            synx <args[0]> FILE <args[1:]> matches FILE.out

FILE.out holds "exit <code>", then the standard output and the standard
error after "--- stdout" and "--- stderr" lines. The fixture path is
written as <file>. --update rewrites the .out files from SYNX.

Files without the header are support modules and are skipped. The standard
library comes from SYNX_STD_PATH, or from std/ in this repository.
"""
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ERROR_LINE = re.compile(r":\d+:\d+: error\b")


def run(synx, args):
    result = subprocess.run([synx, *args], capture_output=True, encoding="utf-8",
                            errors="replace", timeout=600)
    return result.returncode, result.stdout, result.stderr


def header(path):
    lines = path.read_text(encoding="utf-8").splitlines()
    label = lines[0][len("// fixture: "):]
    stage = lines[1][len("// stage: "):] if lines[1].startswith("// stage: ") else ""
    expects = []
    for line in lines[2:]:
        if not line.startswith("// expect: "):
            break
        expects.append(line[len("// expect: "):])
    return label, stage, expects


def return_type(dump, name):
    # Checked function rows: fn#N "name" @span:0 ... "<return type>" [generics] [params]
    for line in dump.splitlines():
        match = re.match(r'fn#\d+ "' + re.escape(name) + r'" @\d+-\d+:0 .*? "([^"]*)" \[', line)
        if match:
            return match.group(1)
    return None


def function_ir(dump, name):
    lines, inside = [], False
    for line in dump.splitlines():
        if line.startswith("fn "):
            inside = line.startswith(f"fn {name}(")
        if inside:
            lines.append(line)
    return "\n".join(lines)


def snapshot(path, code, out, err):
    def clean(text):
        for form in {str(path), path.as_posix(), str(path).replace("\\", "/")}:
            text = text.replace(form, "<file>")
        return text
    return f"exit {code}\n--- stdout\n{clean(out)}--- stderr\n{clean(err)}"


def expectation(synx, path, stage, expect, update):
    kind, _, value = expect.partition(" ")
    file = str(path)
    if kind == "type":
        name, _, want = value.partition(" ")
        code, out, _ = run(synx, ["modules", file, "--check"])
        got = return_type(out, name)
        return None if code == 0 and got == want else f"return type of {name}: want {want}, got {got}"
    if kind == "ok" or (kind == "errors" and value == "0" and stage == "check"):
        args = ["modules", file, "--check"] if stage == "check" else ["check", file]
        code, out, err = run(synx, args)
        return None if code == 0 else f"expected no errors (exit {code})\n{out}{err}"
    if kind in ("error", "errors"):
        code, out, err = run(synx, ["check", file])
        if kind == "error":
            return None if code != 0 and value in out + err else f"expected an error containing '{value}'\n{out}{err}"
        count = len(ERROR_LINE.findall(out + err))
        return None if count == int(value) else f"expected {value} errors, got {count}\n{out}{err}"
    if kind == "parse":
        mode, _, text = value.partition(" ")
        code, out, err = run(synx, ["parse", file])
        if mode == "has":
            return None if code == 0 and text in out else f"expected parse output containing '{text}' (exit {code})\n{out}{err}"
        if mode == "errors":
            count = len(ERROR_LINE.findall(err + out))
            return None if count == int(text) else f"expected {text} parse errors, got {count}\n{err}"
    if kind == "ir":
        words = value.split(" ", 2)
        if words[0] in ("has", "lacks"):
            name, mode, text = None, words[0], value.partition(" ")[2]
        else:
            name, mode, text = words[0], words[1], words[2]
        code, out, err = run(synx, ["ir", file])
        if code != 0:
            return f"synx ir failed (exit {code})\n{err}"
        body = function_ir(out, name) if name else out
        if name and not body:
            return f"no IR for function {name}"
        if (text in body) != (mode == "has"):
            return f"IR{' of ' + name if name else ''} {'lacks' if mode == 'has' else 'contains'} '{text}'\n{body}"
        return None
    if kind == "output":
        args = value.split()
        code, out, err = run(synx, [args[0], file, *args[1:]])
        got = snapshot(path, code, out, err)
        expected_path = path.with_suffix(".out")
        if update:
            expected_path.write_text(got, encoding="utf-8", newline="\n")
            return None
        want = expected_path.read_text(encoding="utf-8") if expected_path.exists() else "(missing .out file)"
        return None if got == want else f"output differs from {expected_path.name}\n--- want\n{want}--- got\n{got}"
    return f"unknown expectation '{expect}'"


def check_fixture(synx, path, update):
    _, stage, expects = header(path)
    if not expects:
        return ["no expectation"]
    problems = [expectation(synx, path, stage, e, update) for e in expects]
    return [p for p in problems if p]


def main():
    args = sys.argv[1:]
    update = "--update" in args
    if update:
        args.remove("--update")
    text_filter = None
    if "--filter" in args:
        at = args.index("--filter")
        text_filter = args[at + 1]
        del args[at:at + 2]
    if len(args) < 2:
        sys.exit(__doc__)
    synx = str(Path(args[0]).resolve())
    os.environ.setdefault("SYNX_STD_PATH", str(ROOT / "std"))
    fixtures = []
    for folder in args[1:]:
        for path in sorted(Path(folder).resolve().rglob("*.snx")):
            with path.open(encoding="utf-8") as stream:
                if stream.readline().startswith("// fixture: "):
                    fixtures.append(path)
    if text_filter:
        fixtures = [p for p in fixtures if text_filter in str(p)]
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        results = list(pool.map(lambda p: (p, check_fixture(synx, p, update)), fixtures))
    failures = 0
    for path, problems in results:
        for problem in problems:
            failures += 1
            name = path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
            print(f"FAIL {name} ({header(path)[0]}): {problem}")
    print(f"fixtures: {len(results)} files, {failures} failures")
    sys.exit(1 if failures or not results else 0)


if __name__ == "__main__":
    main()
