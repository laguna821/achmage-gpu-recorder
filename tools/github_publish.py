"""Use the user's existing Git Credential Manager session; never print credentials."""
import json, os, subprocess, urllib.request, urllib.error, sys

def api(method, path, body=None):
    env = dict(os.environ, GIT_TERMINAL_PROMPT='0', GCM_INTERACTIVE='Never')
    result = subprocess.run(['git','credential','fill'],input='protocol=https\nhost=github.com\n\n',text=True,capture_output=True,env=env,timeout=20)
    values = dict(line.split('=',1) for line in result.stdout.splitlines() if '=' in line)
    token = values.get('password')
    if not token: raise RuntimeError('No existing GitHub credential available')
    payload = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request('https://api.github.com'+path,data=payload,method=method,headers={
        'Authorization':'Bearer '+token,'Accept':'application/vnd.github+json','User-Agent':'Achmage-Recorder-Release','X-GitHub-Api-Version':'2022-11-28'})
    with urllib.request.urlopen(request,timeout=30) as response: return json.load(response)

if __name__ == '__main__':
    if sys.argv[1] == 'whoami':
        user=api('GET','/user'); print(json.dumps({'login':user['login']}))
    elif sys.argv[1] == 'create':
        repo=api('POST','/user/repos',{'name':'achmage-gpu-recorder','description':'Native hotkey display recording for Windows and macOS, inspired by GPU Screen Recorder.','private':False,'auto_init':False})
        print(json.dumps({'url':repo['html_url'],'clone_url':repo['clone_url']}))
