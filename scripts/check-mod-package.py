#!/usr/bin/env python3
"""Fails unless a .geode is this release's mod (id and version from mod.json) and contains the binary."""
import json
import sys
import zipfile
from pathlib import Path

package, binary = sys.argv[1], sys.argv[2]
expected = json.loads((Path(__file__).resolve().parent.parent / 'mod.json').read_text(encoding='utf-8'))
problem = f"{package} is not {expected['id']} {expected['version']} with {binary}. Rebuild the mod before packaging."
try:
    with zipfile.ZipFile(package) as archive:
        names = archive.namelist()
        # zipfile reads the last of two entries with one name while a loader may read the first, and an
        # oversized entry would be read whole into memory.
        if names.count('mod.json') != 1 or archive.getinfo('mod.json').file_size > 65536:
            sys.exit(problem)
        packaged = json.loads(archive.read('mod.json'))
except (zipfile.BadZipFile, KeyError, json.JSONDecodeError):
    sys.exit(problem)
same_mod = packaged.get('id') == expected['id'] and packaged.get('version', '').lstrip('v') == expected['version'].lstrip('v')
if not same_mod or binary not in names:
    sys.exit(problem)
