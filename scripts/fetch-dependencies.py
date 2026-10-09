"""Fetch immutable release inputs without changing existing development dependencies."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def run(*args):
    return subprocess.check_output(args, text=True).strip()

def fetch(platform, destination, include_obs):
    lock = json.loads((ROOT / 'scripts/dependencies.lock.json').read_text())
    destination.mkdir(parents=True, exist_ok=True)
    for name, (url, revision) in lock['sources'].items():
        target = destination / name
        if not target.exists():
            target.mkdir()
            run('git', '-C', str(target), 'init')
            run('git', '-C', str(target), 'remote', 'add', 'origin', url)
            run('git', '-C', str(target), 'fetch', '--depth=1', 'origin', revision)
            run('git', '-C', str(target), 'checkout', '--detach', 'FETCH_HEAD')
            if name == 'geode-sdk':
                run('git', '-C', str(target), 'submodule', 'update', '--init', '--recursive', '--depth=1')
        if not (target / '.git').exists() or run('git', '-C', str(target), 'rev-parse', 'HEAD') != revision:
            raise RuntimeError(f'{target} is not the locked checkout; choose a new destination')
        if run('git', '-C', str(target), 'status', '--porcelain', '--untracked-files=no'):
            raise RuntimeError(f'{target} has local modifications; choose a new destination')
    for name, asset in lock['assets'].items():
        if asset['platform'] not in ('all', platform) or (name.startswith('obs-') and not include_obs):
            continue
        target = destination / asset['destination']
        marker = destination / ('.' + name + '.sha256')
        if target.exists() and marker.exists():
            try:
                recorded = json.loads(marker.read_text())
            except json.JSONDecodeError:
                recorded = {}
            if recorded.get('archive') == asset['sha256']:
                files = recorded.get('files', {})
                if files and all((target / path).is_file() and hashlib.sha256((target / path).read_bytes()).hexdigest() == digest for path, digest in files.items()):
                    continue
                if asset['kind'] == 'file' and hashlib.sha256(target.read_bytes()).hexdigest() == asset['sha256']:
                    continue
            # Older cache markers verified only the archive; re-extract from a fresh verified download.
            if marker.read_text() == asset['sha256']:
                if asset['kind'] == 'file' and hashlib.sha256(target.read_bytes()).hexdigest() == asset['sha256']:
                    marker.write_text(json.dumps({'archive': asset['sha256']}))
                    continue
                raise RuntimeError(f'{target}: old verification marker; choose a new destination')
        if target.exists():
            raise RuntimeError(f'{target} exists without matching verification; choose a new destination')
        with tempfile.TemporaryDirectory(dir=destination, prefix='fetch-') as temp:
            archive = Path(temp) / 'download'
            print('Downloading', name, flush=True)
            request = urllib.request.Request(asset['url'], headers={'User-Agent': 'OBSJukebox-build'})
            with urllib.request.urlopen(request, timeout=120) as response, archive.open('wb') as output:
                shutil.copyfileobj(response, output)
            if hashlib.sha256(archive.read_bytes()).hexdigest() != asset['sha256']:
                raise RuntimeError(f'{name}: SHA256 mismatch')
            if asset['kind'] == 'file':
                shutil.copy2(archive, target)
            else:
                extracted = Path(temp) / 'extracted'
                extracted.mkdir()
                with zipfile.ZipFile(archive) as package:
                    for entry in package.infolist():
                        path = (extracted / entry.filename).resolve()
                        if not path.is_relative_to(extracted.resolve()) or ((entry.external_attr >> 16) & 0o170000) == 0o120000:
                            raise RuntimeError(f'Unsafe archive member: {entry.filename}')
                    package.extractall(extracted)
                extracted.rename(target)
            files = {str(path.relative_to(target)): hashlib.sha256(path.read_bytes()).hexdigest() for path in target.rglob('*') if path.is_file()} if target.is_dir() else {}
            marker.write_text(json.dumps({'archive': asset['sha256'], 'files': files}, sort_keys=True))
    executable = destination / 'geode-cli' / ('geode.exe' if platform == 'windows' else 'geode')
    if executable.exists():
        executable.chmod(executable.stat().st_mode | 0o111)
    print(destination)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('platform', choices=('windows', 'mac'))
    parser.add_argument('--destination', type=Path, default=ROOT / 'tools/pinned')
    parser.add_argument('--include-obs', action='store_true', help='Also fetch the pinned OBS runtime')
    args = parser.parse_args()
    fetch(args.platform, args.destination.resolve(), args.include_obs)
