"""Downloads the Synx release that builds the compiler from source (the seed).

Usage: python scripts/fetch-seed.py DIR

Reads scripts/seed.json, downloads the package for this host into DIR,
checks its SHA-256, unpacks it, and prints the path of the seed compiler.
A package that is already unpacked in DIR is used again. The compiler
source must build with this release; update seed.json after each release
that the compiler source starts to depend on.
"""
import hashlib
import json
import platform
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def host():
    system = platform.system()
    if system == "Windows":
        return "windows"
    if system == "Darwin":
        return "macos"
    if system == "Linux":
        return "linux"
    sys.exit(f"fetch-seed: no Synx seed for {system}; set SYNX_SEED to a Synx compiler")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    seed = json.loads((ROOT / "scripts" / "seed.json").read_text(encoding="utf-8"))
    entry = seed["hosts"][host()]
    folder = Path(sys.argv[1]).resolve() / seed["version"]
    compiler = folder / entry["compiler"]
    marker = folder / ".sha256"
    if compiler.is_file() and marker.is_file() and marker.read_text().strip() == entry["sha256"]:
        print(compiler)
        return
    folder.mkdir(parents=True, exist_ok=True)
    archive = folder / entry["file"]
    if not archive.is_file() or sha256(archive) != entry["sha256"]:
        url = seed["url"] + entry["file"]
        print(f"fetch-seed: downloading {url}", file=sys.stderr)
        with urllib.request.urlopen(url, timeout=300) as response, archive.open("wb") as out:
            while block := response.read(1 << 20):
                out.write(block)
    actual = sha256(archive)
    if actual != entry["sha256"]:
        archive.unlink()
        sys.exit(f"fetch-seed: {entry['file']} has SHA-256 {actual}, expected {entry['sha256']}")
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as package:
            package.extractall(folder)
    else:
        with tarfile.open(archive) as package:
            if hasattr(tarfile, "data_filter"):
                package.extractall(folder, filter="data")
            else:
                package.extractall(folder)
    if not compiler.is_file():
        sys.exit(f"fetch-seed: {entry['file']} has no {entry['compiler']}")
    marker.write_text(entry["sha256"] + "\n")
    print(compiler)


if __name__ == "__main__":
    main()
