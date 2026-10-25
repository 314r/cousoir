#!/usr/bin/env python3
# Looks through the C++ qmlcachegen makes of the QML for a binding or a
# function that can return a value none of its code set: a jump straight to
# the return over the only place that sets what it returns. Qt 6.12 made
# `dark ? { … } : { … }` so, returning the second object whichever branch
# ran, undefined after the first; on a dark page the reader's highlight
# colours were undefined, and each chapter with highlights stayed blank.
#
#   tools/check_compiled_qml.py build/.rcc/qmlcache cousoir_ui_
#
# Prints each one found and exits 1; 0 when there is none.

import glob
import os
import re
import sys

folder = sys.argv[1]
prefix = sys.argv[2] if len(sys.argv) > 2 else ''
found = 0
files = sorted(glob.glob(os.path.join(folder, prefix + '*.cpp')))
if not files:
    print(f'no compiled QML in {folder}')
    sys.exit(1)
for path in files:
    text = open(path, encoding='utf-8').read()
    for block in re.split(r'\n// (?=(?:expression for|function) )', text)[1:]:
        title = block.split('\n', 1)[0]
        ret = block.rfind('// generate_Ret')
        if ret < 0 or 'goto label_' not in block:
            continue
        returned = re.search(r'\*static_cast<[^>]*>\(argv\[0\]\) = (?:std::move\()?(s\.\w+)', block[ret:])
        if not returned:
            continue
        name = re.escape(returned.group(1))
        # Where it is set: assigned, or written through its address.
        sets = [m.start() for m in re.finditer(name + r'\b(?:\.\w+\(\))? = |&' + name + r'\b', block[:ret])]
        for jump in re.finditer(r'goto (label_\d+);', block):
            target = block.find('\n' + jump.group(1) + ':;')
            if target < 0 or target > ret:
                continue
            # Set before the jump, or between where it lands and the return.
            if any(at < jump.start() or target < at for at in sets):
                continue
            print(f'{os.path.basename(path)}: {title}: returns {returned.group(1)}, which a jump to {jump.group(1)} skips')
            found += 1
            break
sys.exit(1 if found else 0)
