"""Portable receipt regression tests; native installer UI tests run on their OS."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--perl', default=shutil.which('perl'))
args = parser.parse_args()
assert args.perl, 'Perl is required'
helper = Path(__file__).resolve().parents[2] / 'scripts/install-state.pl'

def run(*arguments, ok=True):
    result = subprocess.run([args.perl, str(helper), *map(str, arguments)], capture_output=True, text=True)
    assert (result.returncode == 0) == ok, result.stdout + result.stderr
    return result.stdout

with tempfile.TemporaryDirectory(prefix='jukebox-undo-') as temporary:
    root = Path(temporary)
    logs = root / 'logs'
    receipt = logs / '001'
    receipt.mkdir(parents=True)
    backup = receipt / 'old'
    target = root / 'plugin'
    backup.write_bytes(b'old plugin')
    target.write_bytes(b'new plugin')
    run('record', receipt, target, backup)
    # Incomplete installs must never become the next undo target.
    run('undo-latest', logs, ok=False)
    (receipt / 'complete').touch()
    target.write_bytes(b'user edit')
    assert 'Preserved changed file' in run('undo-latest', logs)
    assert target.read_bytes() == b'user edit'
    target.write_bytes(b'new plugin')
    run('undo-latest', logs)
    assert target.read_bytes() == b'old plugin'
    run('undo-latest', logs, ok=False)

    receipt = logs / '002'
    receipt.mkdir()
    (receipt / 'complete').touch()
    bundle = root / 'bundle.plugin'
    bundle.mkdir()
    (bundle / 'binary').write_bytes(b'new')
    run('record', receipt, bundle, receipt / 'absent')
    (bundle / 'user-file').write_bytes(b'keep')
    assert 'Preserved changed file' in run('undo-latest', logs)
    assert (bundle / 'user-file').exists()
    (bundle / 'user-file').unlink()
    run('undo-latest', logs)
    assert not bundle.exists()

    receipt = logs / '003'
    receipt.mkdir()
    (receipt / 'complete').touch()
    override = root / 'flatpak/overrides/com.obsproject.Studio'
    override.parent.mkdir(parents=True)
    backup = receipt / 'override'
    backup.write_text('[Context]\nfilesystems=xdg-pictures:ro;/game;\n')
    override.write_text('[Context]\nfilesystems=xdg-pictures:ro;/game:ro;/prefix:ro;\n')
    run('record', receipt, override, backup)
    override.write_text('[Context]\nfilesystems=xdg-pictures:ro;/game:ro;/prefix:ro;xdg-videos:ro;\n[Environment]\nUSER_SETTING=keep\n')
    run('undo-latest', logs)
    assert override.read_text() == '[Context]\nfilesystems=xdg-pictures:ro;xdg-videos:ro;/game;\n[Environment]\nUSER_SETTING=keep\n'

    receipt = logs / '004'
    receipt.mkdir()
    (receipt / 'complete').touch()
    target.write_bytes(b'new')
    backup = receipt / 'old'
    backup.write_bytes(b'old')
    run('record', receipt, target, backup)
    backup.write_bytes(b'corrupt backup')
    run('undo-latest', logs, ok=False)
    assert target.read_bytes() == b'new'
print('PASS receipt undo: files, directories, changed files, incomplete installs, Flatpak edits, corrupt backups')
