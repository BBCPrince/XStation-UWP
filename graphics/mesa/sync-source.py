"""Copy source overlays by content; changed files must be newer than existing objects."""
from pathlib import Path
import os
import shutil
import sys

destination = Path(sys.argv[1])
selected = {}
for argument in sys.argv[2:]:
    source = Path(argument)
    for path in source.rglob('*'):
        if not path.is_file():
            continue
        selected[path.relative_to(source)] = path
for relative, path in selected.items():
    target = destination / relative
    if target.is_file() and target.read_bytes() == path.read_bytes():
        continue
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(path, target)
    shutil.copymode(path, target)
    os.utime(target, None)
