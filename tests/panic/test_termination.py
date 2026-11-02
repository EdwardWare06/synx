"""Check panic status, direct diagnostics, and absence of C exit-hook cleanup."""
from concurrent.futures import ThreadPoolExecutor
import subprocess
import sys


def launch(mode):
    p = subprocess.run([sys.argv[1], mode], capture_output=True, timeout=20)
    want = 87 if mode == 'normal' else 101
    assert p.returncode == want, (mode, p.returncode, p.stdout, p.stderr)
    assert not p.stdout, p.stdout
    assert p.stderr.replace(b'\r\n', b'\n') == (b'' if mode == 'normal' else b'Synx panic: termination probe\n'), p.stderr


launch('normal')
launch('main')
with ThreadPoolExecutor(max_workers=4) as pool:
    list(pool.map(launch, ['worker'] * 100))
print('panic termination: 102 child processes passed')
