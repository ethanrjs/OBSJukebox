import pathlib, shutil, subprocess, os, json
from paths import ROOT, BASE, ORIGINAL, APP
base=BASE
base.mkdir(parents=True,exist_ok=True)
original=ORIGINAL
app=APP
if not app.exists():
    shutil.copytree(original, app, symlinks=True, ignore=lambda d,n: ['geode'] if pathlib.Path(d)==original/'Contents' else [])
home=base/'home'
home.mkdir(exist_ok=True)
(home/'tmp').mkdir(exist_ok=True)
mods=app/'Contents/geode/mods'
mods.mkdir(parents=True,exist_ok=True)
resources=ROOT/'artifacts/native-mac/preview/OBS Jukebox Setup.app/Contents/Resources/payload/geode/resources'
if not (app/'Contents/geode/resources').exists(): shutil.copytree(resources,app/'Contents/geode/resources')
shutil.copy2(ROOT/'build-trigger-smoke/local.jukebox_trigger_smoke.geode',mods)
profile=base/'isolation.sb'
profile.write_text('(version 1)\n(allow default)\n(deny file-write*)\n(allow file-write* (subpath '+json.dumps(str(base))+') (literal "/dev/null"))\n')
probe='import os; f=os.open('+repr(str(original/'Contents/Frameworks/Geode.dylib'))+',os.O_WRONLY);os.close(f);raise SystemExit(73)'
result=subprocess.run(['/usr/bin/sandbox-exec','-f',str(profile),'/usr/bin/python3','-c',probe],capture_output=True,text=True)
(base/'write-denial.log').write_text(result.stdout+result.stderr)
assert result.returncode!=73 and 'PermissionError' in result.stderr, 'sandbox failed no launch permitted'
subprocess.run(['/usr/bin/sandbox-exec','-f',str(profile),'/usr/bin/touch',str(home/'write-permitted')],check=True)
print('STAGED confinement write denial confirmed',base)