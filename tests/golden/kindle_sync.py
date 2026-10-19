#!/usr/bin/env python3
# Does to a fake Kindle what Amazon does to a Kindle once it syncs: replaces
# the covers of the books sent, in system/thumbnails, with blank ones.
#
#   kindle_sync.py <kindle>

import os
import sys

thumbnails = os.path.join(sys.argv[1], 'system', 'thumbnails')
for name in os.listdir(thumbnails):
    with open(os.path.join(thumbnails, name), 'wb') as f:
        f.write(b'blank')
