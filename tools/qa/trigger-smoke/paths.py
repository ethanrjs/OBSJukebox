from pathlib import Path
import os

ROOT = Path(os.environ.get('OBS_JUKEBOX_ROOT', Path(__file__).resolve().parents[3])).resolve()
BASE = Path(os.environ.get('OBS_JUKEBOX_SMOKE_DIR', ROOT / 'artifacts/trigger-smoke')).resolve()
ORIGINAL = Path(os.environ.get('OBS_JUKEBOX_GAME_APP', Path.home() / 'Library/Application Support/Steam/steamapps/common/Geometry Dash/Geometry Dash.app')).resolve()
APP = BASE / 'Geometry Dash.app'
# Never stage into the installed application, including through a symlink.
if APP.resolve() == ORIGINAL or ORIGINAL in BASE.parents or BASE == ORIGINAL or BASE in ORIGINAL.parents:
    raise RuntimeError('Smoke staging must be separate from the installed game')
if APP.is_symlink():
    raise RuntimeError('Smoke application must not be a symlink')
# A pre-existing staged bundle must not redirect writes through a Geode symlink.
for relative in ('Contents/geode', 'Contents/geode/mods', 'Contents/geode/resources'):
    target = (APP / relative).resolve()
    if APP.resolve() not in target.parents:
        raise RuntimeError('Staged Geode paths must remain inside the copied application')
