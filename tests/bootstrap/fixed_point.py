"""Self-hosting fixed point: stage 2 rebuilds the compiler as stage 3.

Usage: python tests/bootstrap/fixed_point.py STAGE2 WORKDIR

STAGE2 was built by stage 1 with --keep-obj, so STAGE2.o (or .obj) exists.
Stage 3 must emit a byte-identical compiler object, report the same version,
and build and run the workbench example. The runtime capsule beside STAGE2
links stage 3 and the example. Artifacts stay in WORKDIR.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def run(command, env, stdin=None):
    result = subprocess.run([str(c) for c in command], env=env, capture_output=True, input=stdin, timeout=3000)
    if result.returncode:
        sys.stdout.write(result.stdout.decode("utf-8", "replace") + result.stderr.decode("utf-8", "replace"))
        sys.exit(f"bootstrap: exit {result.returncode}: {' '.join(str(c) for c in command)}")
    return result.stdout.decode("utf-8", "replace")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    stage2 = Path(sys.argv[1]).resolve()
    work = Path(sys.argv[2]).resolve()
    work.mkdir(parents=True, exist_ok=True)
    exe = ".exe" if sys.platform == "win32" else ""
    obj = ".obj" if sys.platform == "win32" else ".o"
    capsule = {"win32": "synx_rt.dll", "darwin": "libsynx_rt.dylib"}.get(sys.platform, "libsynx_rt.so")
    env = {k: v for k, v in os.environ.items() if not k.startswith("SYNX_")}
    env["SYNX_STD_PATH"] = str(ROOT / "std")
    env["SYNX_RUNTIME_SHARED"] = str(stage2.parent / capsule)

    stage3 = work / f"synx-stage3{exe}"
    run([stage2, "build", ROOT / "selfhost" / "main.snx", "-o", stage3, "--keep-obj", "--linker=builtin"], env)
    object2 = Path(str(stage2) + obj).read_bytes()
    object3 = Path(str(stage3) + obj).read_bytes()
    if object2 != object3:
        first = next((i for i, (a, b) in enumerate(zip(object2, object3)) if a != b), min(len(object2), len(object3)))
        sys.exit(f"bootstrap: stage 2 and stage 3 objects differ at byte {first} "
                 f"({len(object2)} and {len(object3)} bytes)")
    print(f"identical stage 2 and stage 3 objects ({len(object3)} bytes)")

    version2 = run([stage2, "version"], env)
    version3 = run([stage3, "version"], env)
    if version2 != version3:
        sys.exit(f"bootstrap: versions differ: {version2!r} and {version3!r}")

    workbench = work / f"workbench{exe}"
    run([stage3, "build", ROOT / "examples" / "workbench" / "main.snx", "-o", workbench], env)
    expected = (ROOT / "examples" / "workbench" / "main.expected").read_text(encoding="utf-8")
    stdin = (ROOT / "examples" / "workbench" / "main.stdin").read_bytes()
    output = run([workbench], env, stdin).replace("\r\n", "\n")
    if expected.startswith("exit "):
        expected = expected.partition("\n")[2]
    if output != expected:
        sys.exit(f"bootstrap: workbench output differs\n--- want\n{expected}--- got\n{output}")
    print("stage 3 builds and runs the workbench")


if __name__ == "__main__":
    main()
