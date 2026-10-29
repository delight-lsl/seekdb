#pragma once

static const char *OB_FILE_SQL_PARQUET_SCRIPT = R"SEEKDB(
import gzip
import json
import struct
import sys

try:
    import pyarrow.parquet as parquet
except ImportError:
    parquet = None


class CompactReader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def byte(self):
        value = self.data[self.pos]
        self.pos += 1
        return value

    def varint(self):
        value = shift = 0
        while True:
            byte = self.byte()
            value |= (byte & 127) << shift
            if byte < 128:
                return value
            shift += 7

    def integer(self):
        value = self.varint()
        return (value >> 1) ^ -(value & 1)

    def value(self, kind):
        if kind in (1, 2):
            return kind == 1
        if kind == 3:
            value = self.byte()
            return value - 256 if value > 127 else value
        if kind in (4, 5, 6):
            return self.integer()
        if kind == 7:
            value = struct.unpack_from('<d', self.data, self.pos)[0]
            self.pos += 8
            return value
        if kind == 8:
            size = self.varint()
            value = self.data[self.pos:self.pos + size]
            self.pos += size
            return value.decode('utf-8', 'replace')
        if kind in (9, 10):
            header = self.byte()
            size, item_kind = header >> 4, header & 15
            if size == 15:
                size = self.varint()
            return [self.value(item_kind) for _ in range(size)]
        if kind == 11:
            size = self.varint()
            if not size:
                return {}
            kinds = self.byte()
            key_kind, value_kind = kinds >> 4, kinds & 15
            return {self.value(key_kind): self.value(value_kind) for _ in range(size)}
        if kind == 12:
            return self.struct()
        raise ValueError('unsupported parquet metadata type')

    def struct(self):
        fields = {}
        last_id = 0
        while True:
            header = self.byte()
            if not header:
                return fields
            delta, kind = header >> 4, header & 15
            field_id = last_id + delta if delta else self.integer()
            fields[field_id] = (kind == 1) if kind in (1, 2) else self.value(kind)
            last_id = field_id


def snappy(data):
    pos = 0
    expected = shift = 0
    while True:
        byte = data[pos]
        pos += 1
        expected |= (byte & 127) << shift
        if byte < 128:
            break
        shift += 7
    out = bytearray()
    while pos < len(data):
        tag = data[pos]
        pos += 1
        kind = tag & 3
        if kind == 0:
            size = tag >> 2
            if size < 60:
                size += 1
            else:
                count = size - 59
                size = int.from_bytes(data[pos:pos + count], 'little') + 1
                pos += count
            out.extend(data[pos:pos + size])
            pos += size
        elif kind == 1:
            size = 4 + ((tag >> 2) & 7)
            distance = ((tag & 224) << 3) | data[pos]
            pos += 1
            start = len(out) - distance
            for i in range(size):
                out.append(out[start + i])
        else:
            width = 2 if kind == 2 else 4
            size = 1 + (tag >> 2)
            distance = int.from_bytes(data[pos:pos + width], 'little')
            pos += width
            start = len(out) - distance
            for i in range(size):
                out.append(out[start + i])
    if len(out) != expected:
        raise ValueError('invalid parquet snappy block')
    return bytes(out)


def hybrid_levels(data, count):
    pos = 0
    values = []
    while pos < len(data) and len(values) < count:
        header = 0
        shift = 0
        while True:
            byte = data[pos]
            pos += 1
            header |= (byte & 127) << shift
            if byte < 128:
                break
            shift += 7
        if header & 1:
            groups = header >> 1
            size = groups
            packed = int.from_bytes(data[pos:pos + size], 'little')
            pos += size
            values.extend((packed >> i) & 1 for i in range(min(groups * 8, count - len(values))))
        else:
            run = header >> 1
            value = data[pos] & 1
            pos += 1
            values.extend([value] * min(run, count - len(values)))
    return values


def decode_plain(data, count, physical_type):
    values = []
    pos = 0
    if physical_type in (1, 2):
        size, fmt = (4, '<i') if physical_type == 1 else (8, '<q')
        for _ in range(count):
            values.append(struct.unpack_from(fmt, data, pos)[0])
            pos += size
    elif physical_type in (4, 5):
        size, fmt = (4, '<f') if physical_type == 4 else (8, '<d')
        for _ in range(count):
            values.append(struct.unpack_from(fmt, data, pos)[0])
            pos += size
    elif physical_type == 0:
        values = [(data[i // 8] >> (i % 8)) & 1 != 0 for i in range(count)]
    elif physical_type == 6:
        for _ in range(count):
            size = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            values.append(data[pos:pos + size].decode('utf-8', 'replace'))
            pos += size
    else:
        raise ValueError('unsupported parquet physical type')
    return values


def read_parquet(path):
    data = open(path, 'rb').read()
    if data[:4] != b'PAR1' or data[-4:] != b'PAR1':
        raise ValueError('invalid parquet magic')
    metadata_size = struct.unpack_from('<I', data, len(data) - 8)[0]
    reader = CompactReader(data[len(data) - 8 - metadata_size:len(data) - 8])
    metadata = reader.struct()
    schema = metadata[2]
    names = [node[4] for node in schema[1:]]
    columns = {name: [] for name in names}
    for group in metadata[4]:
        for chunk in group[1]:
            column = chunk[3]
            name = column[3][-1]
            count = column[5]
            page_pos = column[9]
            page_reader = CompactReader(data[page_pos:])
            page_header = page_reader.struct()
            body_pos = page_pos + page_reader.pos
            body = data[body_pos:body_pos + page_header[3]]
            if page_header[1] == 3:
                page = page_header[8]
                num_values, encoding = page[1], page[4]
                def_size, rep_size = page[5], page[6]
                value_pos = def_size + rep_size
                if def_size:
                    levels = hybrid_levels(body[rep_size:value_pos], num_values)
                    present = [level == 1 for level in levels]
                else:
                    present = [True] * num_values
                values_data = body[value_pos:]
                if page[7] and column[4] == 1:
                    values_data = snappy(values_data)
                elif page[7] and column[4] == 2:
                    values_data = gzip.decompress(values_data)
            elif page_header[1] == 0:
                page = page_header[5]
                num_values, encoding = page[1], page[2]
                values_data = snappy(body) if column[4] == 1 else body
                present = [True] * num_values
                value_pos = 0
            else:
                raise ValueError('unsupported parquet page type')
            if encoding != 0:
                raise ValueError('unsupported parquet page encoding')
            values = iter(decode_plain(values_data, sum(present), column[1]))
            columns[name].extend(next(values) if is_present else None for is_present in present)
    rows = [[columns[name][i] for name in names] for i in range(len(next(iter(columns.values()))))]
    return names, rows


if parquet is not None:
    table = parquet.read_table(sys.argv[1])
    names = list(table.column_names)
    rows = [[row.get(name) for name in names] for row in table.to_pylist()]
else:
    names, rows = read_parquet(sys.argv[1])
print(json.dumps(names, ensure_ascii=False))
for row in rows:
    print(json.dumps(row, ensure_ascii=False, default=str))
)SEEKDB";
