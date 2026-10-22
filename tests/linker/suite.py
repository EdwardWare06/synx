#!/usr/bin/env python3
"""Compile one linker fixture probe, then run every linker check for one mode.

Arguments: compiler source-root build-root static-runtime shared-runtime mode.
Temporary artifacts stay under build-root and remain available after failures.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def run(command, env, *, timeout=180):
    command = [str(argument) for argument in command]
    result = subprocess.run(command, env=env, capture_output=True, timeout=timeout)
    if result.stdout:
        print(result.stdout.decode("utf-8", errors="replace"), end="", flush=True)
    if result.stderr:
        print(result.stderr.decode("utf-8", errors="replace"), end="", file=sys.stderr, flush=True)
    if result.returncode:
        raise AssertionError(f"exit {result.returncode}: {command!r}")
    return result


def selected(env, kind):
    result = env.copy()
    result["SYNX_LINK_TEST"] = kind
    return result


def module(path):
    spec = importlib.util.spec_from_file_location("linker_native_checks", path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def emit(probe, directory, kind, env, names):
    snapshots = []
    for _ in range(2):
        result = run([probe, directory], selected(env, kind))
        assert not result.stdout and not result.stderr
        snapshots.append(tuple((directory / name).read_bytes() for name in names))
    assert snapshots[0] == snapshots[1], f"{kind} output changed between identical links"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("source", type=Path)
    parser.add_argument("build", type=Path)
    parser.add_argument("static_runtime", type=Path)
    parser.add_argument("shared_runtime", type=Path)
    parser.add_argument("mode", type=int, choices=range(4))
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    source = args.source.resolve(strict=True)
    build = args.build.resolve(strict=True)
    static_runtime = args.static_runtime.resolve(strict=True)
    shared_runtime = args.shared_runtime.resolve(strict=True)
    tests = source / "tests" / "linker"
    env = os.environ.copy()
    env.pop("SYNX_LINKER", None)
    env.pop("SYNX_LINKER_DRIVER", None)
    env["SYNX_STD_PATH"] = str(source / "std")
    env["SYNX_RUNTIME_LIB"] = str(static_runtime)
    env["SYNX_RUNTIME_SHARED"] = str(shared_runtime)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    work = Path(tempfile.mkdtemp(prefix=f"linker-{args.mode}-", dir=build))
    succeeded = False
    try:
        probe = work / ("link-suite.exe" if os.name == "nt" else "link-suite")
        command = [compiler, "build", source / "selfhost" / "link_suite_probe.snx", "-o", probe, "--linker=system"]
        if args.mode & 1:
            command.append("--no-regalloc")
        if args.mode & 2:
            command.append("--release")
        run(command, env, timeout=900)
        run([sys.executable, "-B", tests / "run.py", probe, "--archive", static_runtime], selected(env, "archive"))
        if sys.platform == "darwin":
            # The old native harness executes ELF on every non-Windows host.
            # Keep its structural checks while macOS runs its own Mach-O image.
            emit(probe, work, "native", env, ("linked.exe", "linked.elf"))
            native = module(tests / "native_test.py")
            native.verify_pe((work / "linked.exe").read_bytes())
            native.verify_elf((work / "linked.elf").read_bytes())
        else:
            run([sys.executable, "-B", tests / "native_test.py", probe], selected(env, "native"))
        run([sys.executable, "-B", tests / "pe_dynamic_test.py", probe], selected(env, "pe"))
        run([sys.executable, "-B", tests / "object_test.py", probe], selected(env, "objects"))
        run([sys.executable, "-B", tests / "elf_object_test.py", probe], selected(env, "elfobjects"))
        emit(probe, work, "elf", env, ("dynamic.elf", "dynamic-empty.elf"))
        if sys.platform.startswith("linux"):
            # The normal fixture imports the runtime capsule and libc. The
            # empty fixture still has dynamic metadata but no dependencies.
            shutil.copy2(shared_runtime, work / "libsynx_rt.so")
            (work / "dynamic.elf").chmod(0o755)
            (work / "dynamic-empty.elf").chmod(0o755)
        for name, libraries in (("dynamic.elf", ("libsynx_rt.so", "libc.so.6")), ("dynamic-empty.elf", ())):
            command = [sys.executable, "-B", tests / "elf_dynamic_test.py", work / name]
            for library in libraries:
                command.extend(("--expect-library", library))
            if sys.platform.startswith("linux"):
                command.extend(("--expect-exit", "42"))
            run(command, env)
        emit(probe, work, "macho", env, ("dynamic-macho",))
        run([sys.executable, "-B", tests / "macho_dynamic_test.py", work / "dynamic-macho"], env)
        if sys.platform == "darwin":
            image = work / "dynamic-macho"
            image.chmod(0o755)
            result = subprocess.run([str(image)], env=env, capture_output=True, timeout=60)
            assert result.returncode == 42, (result.returncode, result.stdout, result.stderr)
            assert not result.stdout and not result.stderr
        print(f"linker suite: mode {args.mode} passed", flush=True)
        succeeded = True
    finally:
        if succeeded:
            shutil.rmtree(work)
        else:
            print(f"linker suite artifacts: {work}", file=sys.stderr, flush=True)


if __name__ == "__main__":
    main()
