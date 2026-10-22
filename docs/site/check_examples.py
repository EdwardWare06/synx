"""Check the examples in the documentation against the compiler.

Usage: python docs/site/check_examples.py [--synx PATH] FILE.md...

For each ```synx block that defines main() and imports no local module, the
script builds the block as example.snx and runs it. The output is compared
with the ```text block that follows, when at most three lines of prose are
between them. A compile error is compared with the compiler's message. The
default compiler is `synx` on PATH. The exit status is 1 if any output
differs.
"""
import argparse
import pathlib
import re
import shutil
import subprocess
import tempfile


def blocks(lines):
    i, out = 0, []
    while i < len(lines):
        m = re.match(r"\s*```(\w*)", lines[i])
        if m:
            indent = len(lines[i]) - len(lines[i].lstrip())
            start, body = i, []
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("```"):
                body.append(lines[i][indent:])
                i += 1
            out.append((m.group(1), start, i, "\n".join(body)))
        i += 1
    return out


def normal(text):
    return "\n".join(l.rstrip() for l in text.replace("\r", "").strip().splitlines())


def run(synx, source):
    folder = pathlib.Path(tempfile.mkdtemp(prefix="synx-doc-"))
    try:
        (folder / "example.snx").write_text(source + "\n", encoding="utf-8")
        build = subprocess.run([synx, "build", "example.snx"], cwd=folder, capture_output=True,
                               text=True, encoding="utf-8", errors="replace")
        if build.returncode != 0:
            return build.stderr
        exe = next(p for p in folder.iterdir() if p.stem == "example" and p.suffix in ("", ".exe"))
        result = subprocess.run([str(exe)], cwd=folder, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", stdin=subprocess.DEVNULL, timeout=120)
        return result.stdout + result.stderr
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--synx", default=shutil.which("synx") or "synx")
    parser.add_argument("files", nargs="+")
    args = parser.parse_args()
    args.synx = str(pathlib.Path(shutil.which(args.synx) or args.synx).resolve())
    compared = differ = 0
    for name in args.files:
        lines = pathlib.Path(name).read_text(encoding="utf-8").splitlines()
        found = blocks(lines)
        for k, (lang, start, end, body) in enumerate(found):
            if lang != "synx" or "main()" not in body or re.search(r"^import (?!std\.)", body, re.M):
                continue
            if k + 1 >= len(found) or found[k + 1][0] != "text":
                continue
            prose = [l for l in lines[end + 1:found[k + 1][1]] if l.strip()]
            if len(prose) > 3:
                continue
            compared += 1
            got = run(args.synx, body)
            if normal(got) != normal(found[k + 1][3]):
                differ += 1
                print(f"--- {name}:{start + 1}\nexpected:\n{found[k + 1][3]}\nactual:\n{got}")
    print(f"{compared} examples compared, {differ} differ")
    return 1 if differ else 0


if __name__ == "__main__":
    raise SystemExit(main())
