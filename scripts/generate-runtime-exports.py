"""Generate runtime export resolvers and a template ABI/catalogue header."""
from pathlib import Path
import hashlib, re, sys
root, out = Path(sys.argv[1]), Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
modules = ['rt', 'threads', 'host', 'driver', 'link', 'cache']
def write(path, text):
    data = text.encode('utf8')
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)

all_names = set()
for module in modules:
    text = (root / ('synx_' + module + '.c' if module != 'rt' else 'synx_rt.c')).read_text()
    names = sorted(set(re.findall(r'^([A-Za-z_][A-Za-z_0-9* ]*?)\b(synx_rt_[a-zA-Z_0-9]+)\s*\([^;{}]*?\)\s*\{', text, re.M)))
    names = sorted({name for prefix, name in names if not prefix.strip().startswith(('static ', 'extern '))})
    all_names.update(names)
    lines = ['/* Generated; definitions and signatures are visible in this translation unit. */',
             'void* synx_export_' + module + '(const char* name) {']
    for name in names: lines.append('  if (!strcmp(name, "' + name + '")) return (void*)(uintptr_t)&' + name + ';')
    lines += ['  return NULL;', '}']
    write(out / ('exports_' + module + '.inc'), '\n'.join(lines) + '\n')
# link_start is conditional, but exists in every shared/template build.
all_names.add('synx_rt_link_start_v1')
catalogue = ''.join(name + '\n' for name in sorted(all_names)).encode()
abi = hashlib.sha256(catalogue).digest()
header = '#define SYNX_EXPORT_ABI {' + ','.join(str(b) for b in abi) + '}\n'
header += '#define SYNX_EXPORT_NAMES_SIZE ' + str(len(catalogue) + 1) + '\n'
header += '#define SYNX_EXPORT_NAMES "' + catalogue.decode().replace('\n', '\\n') + '"\n'
write(out / 'exports_catalogue.h', header)
