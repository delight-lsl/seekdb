#!/usr/bin/env python3
import json
import sys

import pyarrow.parquet as parquet


def main():
    if len(sys.argv) != 2:
        return 2
    table = parquet.read_table(sys.argv[1])
    names = list(table.column_names)
    # Keep a simple, line-oriented protocol so the SQL parser can consume it.
    sys.stdout.write(json.dumps(names, ensure_ascii=False) + "\n")
    rows = table.to_pylist()
    for row in rows:
        values = []
        for name in names:
            value = row.get(name)
            values.append(value)
        sys.stdout.write(json.dumps(values, ensure_ascii=False, default=str) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
