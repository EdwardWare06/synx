"""Independent reader for the compiler's owned source/type/location transport."""
import struct


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def word(self, size, signed=False):
        assert 0 <= self.at <= len(self.data) - size
        value = int.from_bytes(self.data[self.at:self.at + size], 'little', signed=signed)
        self.at += size
        return value

    def string(self):
        size = self.word(4)
        assert size <= len(self.data) - self.at
        text = self.data[self.at:self.at + size].decode('utf8')
        self.at += size
        assert '\0' not in text
        return text


def read(data):
    r = Reader(data)
    magic = data[:8]
    assert magic in (b'SNXLINE1', b'SNXLINE2', b'SNXLINE3')
    r.at = 8
    nf, count = r.word(4), r.word(4)
    files = [r.string() for _ in range(nf)]
    functions = []
    for _ in range(count):
        f = dict(symbol=r.string(), name=r.string(), address=r.word(8), size=r.word(8))
        f['rows'] = [(r.word(8), r.word(4), r.word(4), r.word(4)) for _ in range(r.word(4))]
        f['variables'], f['args'], f['cfi'] = [], [], []
        functions.append(f)
    hashes, types = [], []
    if magic != b'SNXLINE1':
        for _ in range(nf):
            size = r.word(1)
            assert size in (0, 32)
            hashes.append(data[r.at:r.at + size])
            r.at += size
    if magic == b'SNXLINE3':
        for _ in range(r.word(4)):
            start = r.at
            t = dict(kind=r.word(1), name=r.string(), size=r.word(8), inner=r.word(4), count=r.word(8))
            t['_kind_at'], t['_inner_at'] = start, r.at - 12
            t['fields'] = [(r.string(), r.word(4), r.word(8)) for _ in range(r.word(4))]
            assert 0 <= t['kind'] <= 10
            types.append(t)
        for f in functions:
            f['ret'] = r.word(4)
            f['args'] = [r.word(4) for _ in range(r.word(4))]
            for _ in range(r.word(4)):
                v = dict(name=r.string(), type=r.word(4), parameter=r.word(1))
                v['scope'] = [(r.word(8), r.word(8)) for _ in range(r.word(4))]
                count = r.word(4)
                v['_locations_at'] = r.at
                v['locations'] = [(r.word(8), r.word(8), r.word(4, True), r.word(4, True)) for _ in range(count)]
                assert v['type'] < len(types) and v['parameter'] in (0, 1)
                for ranges in (v['scope'], v['locations']):
                    previous = 0
                    for a, b, *_ in ranges:
                        assert previous <= a < b <= f['size']
                        previous = b
                for a, b, reg, offset in v['locations']:
                    assert -1 <= reg <= 15
                    assert any(x <= a < b <= y for x, y in v['scope'])
                f['variables'].append(v)
            count = r.word(4)
            f['_cfi_at'] = r.at
            f['cfi'] = [(r.word(8), r.word(4, True), r.word(4, True)) for _ in range(count)]
        for t in types:
            assert t['inner'] == 0xffffffff or t['inner'] < len(types)
            assert all(ty == 0xffffffff or ty < len(types) for _, ty, _ in t['fields'])
    assert r.at == len(data)
    return dict(files=files, functions=functions, hashes=hashes, types=types)


def lines(data):
    unit = read(data)
    return unit['files'], [(f['symbol'], f['name'], f['address'], f['size'], f['rows']) for f in unit['functions']]


def malformed_values(data):
    unit = read(data)
    cases = []
    if unit['types']:
        t = unit['types'][0]
        # Reject cycles before the CodeView emitter can recurse through them.
        bad = bytearray(data)
        bad[t['_kind_at']] = 5
        struct.pack_into('<I', bad, t['_inner_at'], 0)
        cases.append(bytes(bad))
        bad = bytearray(data)
        struct.pack_into('<I', bad, t['_inner_at'], 0xfffffffe)
        cases.append(bytes(bad))
    for f in unit['functions']:
        if f['cfi']:
            bad = bytearray(data)
            struct.pack_into('<i', bad, f['_cfi_at'] + 12, 7)
            cases.append(bytes(bad))
            break
    for f in unit['functions']:
        for v in f['variables']:
            if v['locations']:
                bad = bytearray(data)
                struct.pack_into('<i', bad, v['_locations_at'] + 16, 16)
                cases.append(bytes(bad))
                return cases
    return cases
