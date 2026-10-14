"""Verify that a naturally exiting parent makes its worker finalize, without killing it."""
import ctypes,json,os,subprocess,sys,time
from ctypes import wintypes as w
from pathlib import Path
k=ctypes.WinDLL('kernel32',use_last_error=True)
u=ctypes.WinDLL('user32',use_last_error=True)
k.GetCurrentProcess.restype=w.HANDLE
k.CloseHandle.argtypes=[w.HANDLE]
k.CreateEventW.argtypes=[w.LPVOID,w.BOOL,w.BOOL,w.LPCWSTR];k.CreateEventW.restype=w.HANDLE
k.DuplicateHandle.argtypes=[w.HANDLE,w.HANDLE,w.HANDLE,ctypes.POINTER(w.HANDLE),w.DWORD,w.BOOL,w.DWORD];k.DuplicateHandle.restype=w.BOOL
k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.OpenProcess.restype=w.HANDLE
k.GetProcessTimes.argtypes=[w.HANDLE]+[ctypes.POINTER(w.FILETIME)]*4;k.GetProcessTimes.restype=w.BOOL
k.WaitForSingleObject.argtypes=[w.HANDLE,w.DWORD];k.WaitForSingleObject.restype=w.DWORD
u.FindWindowW.argtypes=[w.LPCWSTR,w.LPCWSTR];u.FindWindowW.restype=w.HWND
class Point(ctypes.Structure):_fields_=[('x',w.LONG),('y',w.LONG)]
u.MonitorFromPoint.argtypes=[Point,w.DWORD];u.MonitorFromPoint.restype=w.HANDLE

def birth(handle):
    creation=w.FILETIME();other=[w.FILETIME() for _ in range(3)]
    if not k.GetProcessTimes(handle,ctypes.byref(creation),*[ctypes.byref(x) for x in other]):raise ctypes.WinError()
    return creation.dwHighDateTime<<32|creation.dwLowDateTime

if sys.argv[1]=='--parent':
    self=k.GetCurrentProcess();watch=w.HANDLE()
    if not k.DuplicateHandle(self,self,self,ctypes.byref(watch),0x100000,True,0):raise ctypes.WinError()
    point=Point();u.GetCursorPos(ctypes.byref(point));monitor=u.MonitorFromPoint(point,1)
    startup=subprocess.STARTUPINFO();startup.lpAttributeList={'handle_list':[watch.value]}
    child=subprocess.Popen([sys.argv[2],'--worker','--monitor',str(monitor),'--parent-handle',str(watch.value),'--output',sys.argv[3]],
        startupinfo=startup,close_fds=True,creationflags=subprocess.CREATE_NO_WINDOW)
    k.CloseHandle(watch)
    print(json.dumps({'pid':child.pid,'birth':birth(int(child._handle))}),flush=True)
    time.sleep(4)
    # Return normally. Do not signal stop and do not terminate/wait on the child.
else:
    root=Path(__file__).resolve().parent.parent
    if u.FindWindowW('AchmageRecorderBroker01','Achmage Recorder'):raise RuntimeError('Finish existing broker session before this test')
    event=k.CreateEventW(None,True,False,'Local\\AchmageRecorderStop01')
    if not event:raise ctypes.WinError()
    folder=root/'validation/private';folder.mkdir(parents=True,exist_ok=True)
    output=folder/('parent-lifetime-'+str(time.time_ns())+'.mp4')
    parent=subprocess.Popen([sys.executable,__file__,'--parent',str(root/'build/windows/AchmageRecorder.exe'),str(output)],
        stdout=subprocess.PIPE,text=True,creationflags=subprocess.CREATE_NO_WINDOW)
    info=json.loads(parent.stdout.readline())
    handle=k.OpenProcess(0x100000|0x1000,False,info['pid'])
    if not handle or birth(handle)!=info['birth']:raise RuntimeError('Worker identity changed')
    parent.wait(timeout=10)
    if k.WaitForSingleObject(handle,30000)!=0:raise RuntimeError('Worker did not return. No forced termination performed.')
    k.CloseHandle(handle);k.CloseHandle(event)
    result=json.loads((Path(os.environ['LOCALAPPDATA'])/'AchmageRecorder/last-recording.json').read_text(encoding='utf8'))
    if not result.get('success') or Path(result['file'])!=output:raise RuntimeError('Worker did not finalize expected output')
    decoded=subprocess.run([str(root/'build/windows/AchmageRecorder.exe'),'verify',str(output)],capture_output=True,check=True,text=True)
    print(json.dumps({'parentExitedNaturally':True,'workerFinalized':True,'decode':json.loads(decoded.stdout)}))
