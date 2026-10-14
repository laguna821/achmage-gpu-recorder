"""Use the user's existing Git Credential Manager session; never print credentials."""
import json, os, subprocess, urllib.request, urllib.error, sys
from pathlib import Path

def credential():
    env = dict(os.environ, GIT_TERMINAL_PROMPT='0', GCM_INTERACTIVE='Never')
    result = subprocess.run(['git','credential','fill'],input='protocol=https\nhost=github.com\n\n',text=True,capture_output=True,env=env,timeout=20)
    values = dict(line.split('=',1) for line in result.stdout.splitlines() if '=' in line)
    if not values.get('password'):raise RuntimeError('No existing GitHub credential available')
    return values['password']

def api(method, path, body=None):
    token = credential()
    payload = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request('https://api.github.com'+path,data=payload,method=method,headers={
        'Authorization':'Bearer '+token,'Accept':'application/vnd.github+json','User-Agent':'Achmage-Recorder-Release','X-GitHub-Api-Version':'2022-11-28'})
    with urllib.request.urlopen(request,timeout=30) as response: return json.load(response)

def artifact(artifact_id, destination):
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self,*args,**kwargs):return None
    request=urllib.request.Request('https://api.github.com/repos/laguna821/achmage-gpu-recorder/actions/artifacts/'+str(artifact_id)+'/zip',headers={'Authorization':'Bearer '+credential(),'User-Agent':'Achmage-Recorder'})
    try:
        with urllib.request.build_opener(NoRedirect).open(request,timeout=30) as r:data=r.read()
    except urllib.error.HTTPError as e:
        if e.code!=302:raise
        location=e.headers['Location']
        from urllib.parse import urlparse
        host=urlparse(location).hostname or ''
        if not location.startswith('https://') or not (host.endswith('.blob.core.windows.net') or host.endswith('.githubusercontent.com')):
            raise RuntimeError('Unexpected artifact redirect host')
        # Never forward the GitHub bearer credential to the artifact host.
        with urllib.request.urlopen(location,timeout=60) as r:data=r.read()
    Path(destination).write_bytes(data)

def upload(release_id, path):
    from urllib.parse import quote
    file=Path(path);data=file.read_bytes()
    req=urllib.request.Request('https://uploads.github.com/repos/laguna821/achmage-gpu-recorder/releases/'+str(release_id)+'/assets?name='+quote(file.name),
        data=data,method='POST',headers={'Authorization':'Bearer '+credential(),'User-Agent':'Achmage-Recorder','Content-Type':'application/octet-stream'})
    with urllib.request.urlopen(req,timeout=60) as r:result=json.load(r)
    return result['browser_download_url']

if __name__ == '__main__':
    if sys.argv[1] == 'whoami':
        user=api('GET','/user'); print(json.dumps({'login':user['login']}))
    elif sys.argv[1] == 'create':
        repo=api('POST','/user/repos',{'name':'achmage-gpu-recorder','description':'Native hotkey display recording for Windows and macOS, inspired by GPU Screen Recorder.','private':False,'auto_init':False})
        print(json.dumps({'url':repo['html_url'],'clone_url':repo['clone_url']}))
