#!/usr/bin/env python3
# Compares what `reliure-cli read` and tools/oracle/read_oracle.py read from
# the same files, field by field.
#
#   compare_reads.py <calibre.json> <reliure.json>

import json
import sys

calibre = json.load(open(sys.argv[1], encoding='utf-8'))
reliure = json.load(open(sys.argv[2], encoding='utf-8'))
if len(calibre) != len(reliure):
    sys.exit(f'{len(calibre)} books read by Calibre, {len(reliure)} by Reliure')

def normal(field, value):
    if field == 'identifiers':
        return dict(sorted(value.items()))
    if field in ('series_index', 'rating') and value is not None:
        return float(value)
    return value

differences = 0
fields = {}
for ours, theirs in zip(reliure, calibre):
    for field in theirs:
        a, b = normal(field, ours.get(field)), normal(field, theirs[field])
        if a != b:
            differences += 1
            fields[field] = fields.get(field, 0) + 1
            print(f'{theirs["file"]}: {field}\n    calibre {b!r}\n    reliure {a!r}')
print(f'\n{len(calibre)} files, {differences} differences', fields or '')
sys.exit(1 if differences else 0)
