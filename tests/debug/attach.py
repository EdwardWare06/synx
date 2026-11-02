"""Become GDB after launching its child, to permit Linux ptrace without sudo."""
import os
import subprocess
import sys

child = subprocess.Popen([sys.argv[1]], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
try:
    ready = child.stdout.readline()
    if ready != b'READY\n':
        raise RuntimeError((ready, child.stderr.read()))
    os.execvp('gdb', ['gdb', '-q', '-nx', '-batch', '-iex', 'set debuginfod enabled off',
                      '-p', str(child.pid), '-x', sys.argv[2]])
finally:
    # exec replaces this process. This path runs only if launching GDB failed.
    child.kill()
    child.wait()
