"""Generate deterministic, dependency-free multi-page/RG Parquet test fixtures."""
import struct
import sys
from pathlib import Path

def varint(n):
    out = bytearray()
    while n >= 128:
        out.append((n & 127) | 128)
        n >>= 7
    out.append(n)
    return bytes(out)

def integer(n):
    return varint((n << 1) ^ (n >> 63))

def field(i, t, payload):
    return bytes([t]) + integer(i) + payload

def obj(*fields):
    return b''.join(fields) + b'\0'

def items(t, values):
    return (bytes([(len(values) << 4) | t]) if len(values) < 15 else bytes([240 | t]) + varint(len(values))) + b''.join(values)

def text(s):
    return varint(len(s)) + s.encode()

def page(kind, count, encoding, payload):
    sub = obj(field(1, 5, integer(count)), field(2, 5, integer(encoding)))
    if kind == 0:
        sub += b''
        # Optional level encoding fields are ignored by this reader.
    header = obj(field(1, 5, integer(kind)), field(2, 5, integer(len(payload))),
                 field(3, 5, integer(len(payload))), field(7 if kind == 2 else 5, 12, sub))
    return header + payload

def generate(path, corrupt_unused=False):
    data = bytearray(b'PAR1')
    groups = []
    for g in range(3):
        chunks = []
        for c in range(2):
            start = len(data)
            dictionary = [f'g{g}a', f'g{g}b']
            body = b''.join(struct.pack('<I', len(s)) + s.encode() for s in dictionary)
            data.extend(page(2, 2, 0, body))
            data_offset = len(data)
            for p in range(2):
                # Eight rows per page, alternating NULL/present. Definition
                # levels and dictionary indices both use bitpacked groups.
                levels = bytes([3, 0xaa])
                indices = bytes([1, 3, 0x0a])
                body = struct.pack('<I', len(levels)) + levels + indices
                data.extend(page(0, 8, 8, body))
            if corrupt_unused and c == 1:
                data[start] = 0xff
            size = len(data) - start
            meta = obj(field(1, 5, integer(6)), field(2, 9, items(5, [integer(0), integer(8)])),
                       field(3, 9, items(8, [text(f'c{c}')])), field(4, 5, integer(0)),
                       field(5, 6, integer(16)), field(6, 6, integer(size)), field(7, 6, integer(size)),
                       field(9, 6, integer(data_offset)), field(11, 6, integer(start)))
            chunks.append(obj(field(2, 6, integer(start)), field(3, 12, meta)))
        groups.append(obj(field(1, 9, items(12, chunks)), field(2, 6, integer(0)), field(3, 6, integer(16))))
    schema = [obj(field(4, 8, text('schema')), field(5, 5, integer(2)))]
    for c in range(2):
        schema.append(obj(field(1, 5, integer(6)), field(3, 5, integer(1)), field(4, 8, text(f'c{c}'))))
    footer = obj(field(1, 5, integer(1)), field(2, 9, items(12, schema)),
                 field(3, 6, integer(48)), field(4, 9, items(12, groups)))
    data.extend(footer + struct.pack('<I', len(footer)) + b'PAR1')
    Path(path).write_bytes(data)

def generate_logical(path, unit):
    is_date = unit == 0
    physical = 1 if is_date else 2
    logical = obj(field(6, 12, obj())) if is_date else obj(field(8, 12, obj(field(1, 1, b''), field(2, 12, obj(field(unit, 12, obj()))))))
    schema = [obj(field(4, 8, text('schema')), field(5, 5, integer(1))),
              obj(field(1, 5, integer(physical)), field(3, 5, integer(0)), field(4, 8, text('value')), field(10, 12, logical))]
    body = struct.pack('<i', 1) if is_date else struct.pack('<q', 1000 if unit == 1 else 1000000)
    chunk = page(0, 1, 0, body)
    meta = obj(field(1, 5, integer(physical)), field(4, 5, integer(0)), field(5, 6, integer(1)),
               field(6, 6, integer(len(chunk))), field(7, 6, integer(len(chunk))), field(9, 6, integer(4)))
    group = obj(field(1, 9, items(12, [obj(field(3, 12, meta))])), field(3, 6, integer(1)))
    footer = obj(field(1, 5, integer(1)), field(2, 9, items(12, schema)), field(3, 6, integer(1)), field(4, 9, items(12, [group])))
    Path(path).write_bytes(b'PAR1' + chunk + footer + struct.pack('<I', len(footer)) + b'PAR1')

if __name__ == '__main__':
    generate(sys.argv[1])
    generate(sys.argv[1] + '.corrupt-unused', True)
    for unit in range(4):
        generate_logical(sys.argv[1] + '.logical' + str(unit), unit)
