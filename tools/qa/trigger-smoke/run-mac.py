import os, pathlib, subprocess, json, time
base=pathlib.Path('/Users/home/projects/obsjukebox-validation-6zJJ9T/artifacts/trigger-smoke')
app=base/'Geometry Dash.app'
r=subprocess.run(['/usr/bin/pgrep','-x','Geometry Dash'],capture_output=True,text=True)
if r.returncode==0: raise SystemExit('Geometry Dash already running; no launch')
env=dict(os.environ,HOME=str(base/'home'),CFFIXED_USER_HOME=str(base/'home'),TMPDIR=str(base/'home/tmp'),OBS_JUKEBOX_QA_HOME=str(base/'home'),OBS_JUKEBOX_TEST_PORT='39032',SteamAppId='322170')
log=base/'launch.log'
with log.open('w') as out:
 p=subprocess.Popen(['/usr/bin/sandbox-exec','-f',str(base/'isolation.sb'),str(app/'Contents/MacOS/Geometry Dash')],cwd=app/'Contents',env=env,stdout=out,stderr=subprocess.STDOUT)
 try: code=p.wait(timeout=48)
 except subprocess.TimeoutExpired:
  p.terminate()
  try: code=p.wait(timeout=3)
  except subprocess.TimeoutExpired: p.kill();code=p.wait()
  print('TIMEOUT owned child stopped',p.pid)
 print('exit',code,'pid',p.pid)
print(log.read_text()[-6000:])
smoke=base/'home/trigger-smoke.log'
if smoke.exists(): print(smoke.read_text()[:3000],smoke.read_text()[-1000:])
raise SystemExit(0 if code == 0 and smoke.exists() and 'FINISHED' in smoke.read_text() else 1)
